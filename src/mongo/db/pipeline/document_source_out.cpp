/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/pipeline/document_source_out_in_place.h"
#include "mongo/db/pipeline/document_source_out_replace_coll.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

std::unique_ptr<LiteParsedDocumentSourceForeignCollections> DocumentSourceOut::liteParse(
    const AggregationRequest& request, const BSONElement& spec) {

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$out stage requires a string or object argument, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    NamespaceString targetNss;
    bool allowSharded;
    if (spec.type() == BSONType::String) {
        targetNss = NamespaceString(request.getNamespaceString().db(), spec.valueStringData());
        allowSharded = false;
    } else if (spec.type() == BSONType::Object) {
        auto outSpec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), spec.embeddedObject());

        if (auto targetDb = outSpec.getTargetDb()) {
            targetNss = NamespaceString(*targetDb, outSpec.getTargetCollection());
        } else {
            targetNss =
                NamespaceString(request.getNamespaceString().db(), outSpec.getTargetCollection());
        }

        // Sharded output collections are not allowed with mode "replaceCollection".
        allowSharded = outSpec.getMode() != WriteModeEnum::kModeReplaceCollection;
    }

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid $out target namespace, " << targetNss.ns(),
            targetNss.isValid());

    ActionSet actions{ActionType::remove, ActionType::insert};
    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{Privilege(ResourcePattern::forExactNamespace(targetNss), actions)};

    return stdx::make_unique<LiteParsedDocumentSourceForeignCollections>(
        std::move(targetNss), std::move(privileges), allowSharded);
}

REGISTER_DOCUMENT_SOURCE(out, DocumentSourceOut::liteParse, DocumentSourceOut::createFromBson);

const char* DocumentSourceOut::getSourceName() const {
    return "$out";
}

DocumentSource::GetNextResult DocumentSourceOut::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    if (!_initialized) {
        initializeWriteNs();
        _initialized = true;
    }

    BatchedObjects batch;
    int bufferedBytes = 0;

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        auto doc = nextInput.releaseDocument();

        // Extract the unique key before converting the document to BSON.
        auto uniqueKey = document_path_support::extractPathsFromDoc(doc, _uniqueKeyFields);
        auto insertObj = doc.toBson();
        uassert(50905,
                str::stream() << "Failed to extract unique key from input document: " << insertObj,
                uniqueKey.size() == _uniqueKeyFields.size());

        bufferedBytes += insertObj.objsize();
        if (!batch.empty() &&
            (bufferedBytes > BSONObjMaxUserSize || batch.size() >= write_ops::kMaxWriteBatchSize)) {
            spill(batch);
            batch.clear();
            bufferedBytes = insertObj.objsize();
        }
        batch.emplace(std::move(insertObj), uniqueKey.toBson());
    }
    if (!batch.empty())
        spill(batch);

    switch (nextInput.getStatus()) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case GetNextResult::ReturnStatus::kPauseExecution: {
            return nextInput;  // Propagate the pause.
        }
        case GetNextResult::ReturnStatus::kEOF: {

            finalize();
            _done = true;

            // $out doesn't currently produce any outputs.
            return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

DocumentSourceOut::DocumentSourceOut(const NamespaceString& outputNs,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     WriteModeEnum mode,
                                     std::set<FieldPath> uniqueKey)
    : DocumentSource(expCtx),
      _done(false),
      _outputNs(outputNs),
      _mode(mode),
      _uniqueKeyFields(std::move(uniqueKey)) {}

intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "$out cannot be used in a transaction",
            !expCtx->inMultiDocumentTransaction);

    auto readConcernLevel = repl::ReadConcernArgs::get(expCtx->opCtx).getLevel();
    uassert(ErrorCodes::InvalidOptions,
            "$out cannot be used with a 'majority' read concern level",
            readConcernLevel != repl::ReadConcernLevel::kMajorityReadConcern);

    auto mode = WriteModeEnum::kModeReplaceCollection;
    std::set<FieldPath> uniqueKey;
    NamespaceString outputNs;
    if (elem.type() == BSONType::String) {
        outputNs = NamespaceString(expCtx->ns.db().toString() + '.' + elem.str());
        uniqueKey.emplace("_id");
    } else if (elem.type() == BSONType::Object) {
        auto spec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), elem.embeddedObject());

        mode = spec.getMode();

        // Convert unique key object to a vector of FieldPaths.
        if (auto uniqueKeyObj = spec.getUniqueKey()) {
            uniqueKey = uniqueKeyObj->getFieldNames<std::set<FieldPath>>();
        } else {
            // TODO SERVER-35954: If not present, build the unique key from the shard key of the
            // output collection.
            uniqueKey.emplace("_id");
        }

        // Retrieve the target database from the user command, otherwise use the namespace from the
        // expression context.
        if (auto targetDb = spec.getTargetDb()) {
            outputNs = NamespaceString(*targetDb, spec.getTargetCollection());
        } else {
            outputNs = NamespaceString(expCtx->ns.db(), spec.getTargetCollection());
        }
    } else {
        uasserted(16990,
                  str::stream() << "$out only supports a string or object argument, not "
                                << typeName(elem.type()));
    }

    // Although we perform a check for "replaceCollection" mode with a sharded output collection
    // during lite parsing, we need to do it here as well in case mongos is stale or the command is
    // sent directly to the shard.
    uassert(17017,
            str::stream() << "$out with mode " << WriteMode_serializer(mode)
                          << " is not supported to an existing *sharded* output collection.",
            !(mode == WriteModeEnum::kModeReplaceCollection &&
              expCtx->mongoProcessInterface->isSharded(expCtx->opCtx, outputNs)));

    uassert(17385, "Can't $out to special collection: " + outputNs.coll(), !outputNs.isSpecial());

    switch (mode) {
        case WriteModeEnum::kModeReplaceCollection:
            return new DocumentSourceOutReplaceColl(outputNs, expCtx, mode, uniqueKey);
        case WriteModeEnum::kModeInsertDocuments:
            return new DocumentSourceOutInPlace(outputNs, expCtx, mode, uniqueKey);
        case WriteModeEnum::kModeReplaceDocuments:
            return new DocumentSourceOutInPlaceReplace(outputNs, expCtx, mode, uniqueKey);
        default:
            MONGO_UNREACHABLE;
    }
}

Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument serialized(
        Document{{DocumentSourceOutSpec::kTargetCollectionFieldName, _outputNs.coll()},
                 {DocumentSourceOutSpec::kTargetDbFieldName, _outputNs.db()},
                 {DocumentSourceOutSpec::kModeFieldName, WriteMode_serializer(_mode)}});
    BSONObjBuilder uniqueKeyBob;
    for (auto path : _uniqueKeyFields) {
        uniqueKeyBob.append(path.fullPath(), 1);
    }
    serialized[DocumentSourceOutSpec::kUniqueKeyFieldName] = Value(uniqueKeyBob.done());
    return Value(Document{{getSourceName(), serialized.freeze()}});
}

DepsTracker::State DocumentSourceOut::getDependencies(DepsTracker* deps) const {
    deps->needWholeDocument = true;
    return DepsTracker::State::EXHAUSTIVE_ALL;
}
}  // namespace mongo
