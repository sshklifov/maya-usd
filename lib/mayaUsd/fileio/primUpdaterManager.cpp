//
// Copyright 2021 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "primUpdaterManager.h"

#include <mayaUsd/fileio/fallbackPrimUpdater.h>
#include <mayaUsd/fileio/importData.h>
#include <mayaUsd/fileio/jobs/jobArgs.h>
#include <mayaUsd/fileio/jobs/readJob.h>
#include <mayaUsd/fileio/jobs/writeJob.h>
#include <mayaUsd/fileio/primUpdaterRegistry.h>
#include <mayaUsd/fileio/utils/writeUtil.h>
#include <mayaUsd/nodes/proxyShapeBase.h>
#include <mayaUsd/ufe/Global.h>
#include <mayaUsd/ufe/Utils.h>
#include <mayaUsd/undo/OpUndoItemMuting.h>
#include <mayaUsd/undo/OpUndoItems.h>
#include <mayaUsd/undo/UsdUndoBlock.h>
#include <mayaUsd/utils/traverseLayer.h>
#include <mayaUsdUtils/util.h>

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/instantiateSingleton.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>

#include <maya/MAnimControl.h>
#include <maya/MDagModifier.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnSet.h>
#include <maya/MFnStringData.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MItDag.h>
#include <maya/MSceneMessage.h>
#include <ufe/globalSelection.h>
#include <ufe/hierarchy.h>
#include <ufe/observableSelection.h>
#include <ufe/path.h>
#include <ufe/pathString.h>
#include <ufe/sceneNotification.h>

#include <functional>
#include <tuple>

using UpdaterFactoryFn = UsdMayaPrimUpdaterRegistry::UpdaterFactoryFn;
using namespace MayaUsd;

// Allow for use of MObjectHandle with std::unordered_map.
namespace std {
template <> struct hash<MObjectHandle>
{
    std::size_t operator()(const MObjectHandle& obj) const { return obj.hashCode(); }
};
} // namespace std

namespace MAYAUSD_NS_DEF {
namespace ufe {

//------------------------------------------------------------------------------
// Global variables
//------------------------------------------------------------------------------
extern Ufe::Rtid g_MayaRtid;

} // namespace ufe
} // namespace MAYAUSD_NS_DEF

namespace {

const std::string kPullParentPathKey("Maya:Pull:ParentPath");

// Set name that will be used to hold all pulled objects
const MString kPullSetName("usdEditAsMaya");

// Metadata key used to store pull information on a prim
const TfToken kPullPrimMetadataKey("Maya:Pull:DagPath");

// Metadata key used to store pull information on a DG node
const MString kPullDGMetadataKey("Pull_UfePath");

// Name of Dag node under which all pulled sub-hierarchies are rooted.
const MString kPullRootName("__mayaUsd__");

MObject findPullRoot()
{
    // Try to find one in the scene.
    auto       worldObj = MItDag().root();
    MFnDagNode world(worldObj);
    auto       nbWorldChildren = world.childCount();
    for (unsigned int i = 0; i < nbWorldChildren; ++i) {
        auto              childObj = world.child(i);
        MFnDependencyNode child(childObj);
        if (child.name() == kPullRootName) {
            return childObj;
        }
    }

    return MObject();
}

Ufe::Path usdToMaya(const Ufe::Path& usdPath)
{
    auto prim = MayaUsd::ufe::ufePathToPrim(usdPath);
    if (!TF_VERIFY(prim)) {
        return Ufe::Path();
    }
    std::string dagPathStr;
    if (!TF_VERIFY(PXR_NS::PrimUpdaterManager::readPullInformation(prim, dagPathStr))) {
        return Ufe::Path();
    }

    return Ufe::PathString::path(dagPathStr);
}

SdfPath makeDstPath(const SdfPath& dstRootParentPath, const SdfPath& srcPath)
{
    auto relativeSrcPath = srcPath.MakeRelativePath(SdfPath::AbsoluteRootPath());
    return dstRootParentPath.AppendPath(relativeSrcPath);
}

//------------------------------------------------------------------------------
//
// Verify if the given prim under the given UFE path is an ancestor of an already edited prim.
bool hasEditedDescendant(const Ufe::Path& ufeQueryPath)
{
    MObject pullSetObj;
    auto    status = UsdMayaUtil::GetMObjectByName(kPullSetName, pullSetObj);
    if (status != MStatus::kSuccess)
        return false;

    MFnSet         fnPullSet(pullSetObj);
    MSelectionList members;
    const bool     flatten = true;
    fnPullSet.getMembers(members, flatten);

    for (unsigned int i = 0; i < members.length(); ++i) {
        MDagPath pulledDagPath;
        members.getDagPath(i, pulledDagPath);
        Ufe::Path pulledUfePath;
        if (!PrimUpdaterManager::readPullInformation(pulledDagPath, pulledUfePath))
            continue;

        if (pulledUfePath.startsWith(ufeQueryPath))
            return true;
    }

    return false;
}

//------------------------------------------------------------------------------
//
// The UFE path is to the pulled prim, and the Dag path is the corresponding
// Maya pulled object.
bool writePullInformation(const Ufe::Path& ufePulledPath, const MDagPath& path)
{
    auto pulledPrim = MayaUsd::ufe::ufePathToPrim(ufePulledPath);
    if (!pulledPrim) {
        return false;
    }

    // Add to a set, the set should already been created.
    if (!FunctionUndoItem::execute(
            "Add edited item to pull set.",
            [path]() {
                MObject pullSetObj;
                auto    status = UsdMayaUtil::GetMObjectByName(kPullSetName, pullSetObj);
                if (status != MStatus::kSuccess)
                    return false;
                MFnSet fnPullSet(pullSetObj);
                fnPullSet.addMember(path);
                return true;
            },
            [path]() {
                MObject pullSetObj;
                auto    status = UsdMayaUtil::GetMObjectByName(kPullSetName, pullSetObj);
                if (status != MStatus::kSuccess)
                    return false;
                MFnSet fnPullSet(pullSetObj);
                fnPullSet.removeMember(path, MObject::kNullObj);
                return true;
            })) {
        TF_WARN("Cannot edited object to pulled set.");
        return false;
    }

    // Store metadata on the prim in the Session Layer.
    auto stage = pulledPrim.GetStage();
    if (!stage)
        return false;
    UsdEditContext editContext(stage, stage->GetSessionLayer());
    VtValue        value(path.fullPathName().asChar());
    if (!pulledPrim.SetMetadataByDictKey(SdfFieldKeys->CustomData, kPullPrimMetadataKey, value))
        return false;

    // Store medata on DG node
    auto              ufePathString = Ufe::PathString::string(ufePulledPath);
    MFnDependencyNode depNode(path.node());
    MStatus           status;
    MPlug             dgMetadata = depNode.findPlug(kPullDGMetadataKey, &status);
    if (status != MStatus::kSuccess) {
        MFnStringData fnStringData;
        MObject       strAttrObject = fnStringData.create("");

        MFnTypedAttribute attr;
        MObject           attrObj
            = attr.create(kPullDGMetadataKey, kPullDGMetadataKey, MFnData::kString, strAttrObject);
        status = depNode.addAttribute(attrObj);
        dgMetadata = depNode.findPlug(kPullDGMetadataKey, &status);
        if (status != MStatus::kSuccess) {
            return false;
        }
    }
    dgMetadata.setValue(ufePathString.c_str());

    return true;
}

//------------------------------------------------------------------------------
//
void removePullInformation(const Ufe::Path& ufePulledPath)
{
    UsdPrim prim = MayaUsd::ufe::ufePathToPrim(ufePulledPath);
    auto    stage = prim.GetStage();
    if (!stage)
        return;
    UsdEditContext editContext(stage, stage->GetSessionLayer());
    prim.ClearCustomDataByKey(kPullPrimMetadataKey);

    // Session layer cleanup
    for (const SdfPrimSpecHandle& rootPrimSpec : stage->GetSessionLayer()->GetRootPrims()) {
        stage->GetSessionLayer()->RemovePrimIfInert(rootPrimSpec);
    }
}

//------------------------------------------------------------------------------
//
bool addExcludeFromRendering(const Ufe::Path& ufePulledPath)
{
    UsdPrim prim = MayaUsd::ufe::ufePathToPrim(ufePulledPath);

    auto stage = prim.GetStage();
    if (!stage)
        return false;

    UsdEditContext editContext(stage, stage->GetSessionLayer());
    if (!prim.SetActive(false))
        return false;

    return true;
}

//------------------------------------------------------------------------------
//
bool removeExcludeFromRendering(const Ufe::Path& ufePulledPath)
{
    UsdPrim prim = MayaUsd::ufe::ufePathToPrim(ufePulledPath);

    auto stage = prim.GetStage();
    if (!stage)
        return false;

    SdfLayerHandle sessionLayer = stage->GetSessionLayer();
    UsdEditContext editContext(stage, sessionLayer);

    // Cleanup the field and potentially empty over
    if (!prim.ClearActive())
        return false;

    SdfPrimSpecHandle primSpec = MayaUsdUtils::getPrimSpecAtEditTarget(prim);
    if (sessionLayer && primSpec)
        sessionLayer->ScheduleRemoveIfInert(primSpec.GetSpec());

    return true;
}

//------------------------------------------------------------------------------
//
// Turn on the mesh flag to allow topological modifications.
bool allowTopologyModifications(MDagPath& root)
{
    MDGModifier& dgMod = MDGModifierUndoItem::create("Allow topology modifications");

    MItDag dagIt;
    dagIt.reset(root, MItDag::kDepthFirst, MFn::kMesh);
    for (; !dagIt.isDone(); dagIt.next()) {
        MFnDependencyNode depNode(dagIt.item());
        MPlug             topoPlug = depNode.findPlug("allowTopologyMod");
        if (topoPlug.isNull())
            continue;
        dgMod.newPlugValueBool(topoPlug, true);
    }

    return dgMod.doIt();
}

//------------------------------------------------------------------------------
//
// Perform the import step of the pull (first step), with the argument
// prim as the root of the USD hierarchy to be pulled.  The UFE path and
// the prim refer to the same object: the prim is passed in as an
// optimization to avoid an additional call to ufePathToPrim().
using PullImportPaths = std::pair<std::vector<MDagPath>, std::vector<Ufe::Path>>;
PullImportPaths pullImport(
    const Ufe::Path&                 ufePulledPath,
    const UsdPrim&                   pulledPrim,
    const UsdMayaPrimUpdaterContext& context)
{
    std::string mFileName = context.GetUsdStage()->GetRootLayer()->GetIdentifier();
    if (mFileName.empty()) {
        TF_WARN("Nothing to edit: invalid layer.");
        return PullImportPaths();
    }

    VtDictionary userArgs(context.GetUserArgs());
    userArgs[UsdMayaJobImportArgsTokens->pullImportStage] = PXR_NS::VtValue(context.GetUsdStage());

    UsdMayaJobImportArgs jobArgs = UsdMayaJobImportArgs::CreateFromDictionary(
        userArgs,
        /* importWithProxyShapes = */ false,
        GfInterval::GetFullInterval());

    MayaUsd::ImportData importData(mFileName);
    importData.setRootPrimPath(pulledPrim.GetPath().GetText());

    auto readJob = std::make_shared<UsdMaya_ReadJob>(importData, jobArgs);

    MDagPath pullParentPath;
    {
        auto found = userArgs.find(kPullParentPathKey);
        if (found != userArgs.end()) {
            const std::string& dagPathStr = found->second.Get<std::string>();
            pullParentPath = UsdMayaUtil::nameToDagPath(dagPathStr);
            if (pullParentPath.isValid()) {
                readJob->SetMayaRootDagPath(pullParentPath);
            }
        }
    }

    std::vector<MDagPath>  addedDagPaths;
    std::vector<Ufe::Path> pulledUfePaths;

    // Execute the command, which can succeed but import nothing.
    bool success = readJob->Read(&addedDagPaths);
    if (!success || addedDagPaths.size() == 0) {
        TF_WARN("Nothing to edit in the selection.");
        return PullImportPaths({}, {});
    }

    // Note: UsdMaya_ReadJob has explicit Read(), Undo() and Redo() functions,
    //       and Read() has already been called, so create the function-undo item
    //       but do not execute it.
    FunctionUndoItem::create(
        "Edit as Maya USD import",
        [readJob]() { return readJob->Redo(); },
        [readJob]() { return readJob->Undo(); });

    MDagPath addedDagPath = addedDagPaths[0];

    const bool isCopy = context.GetArgs()._copyOperation;
    if (!isCopy) {
        // Quick workaround to reuse some POC code - to rewrite later

        // Communication to current proxyAccessor code is through the global
        // selection, so we must save the current selection for proper undo.
        // This is not logically necessary, and should be re-written to avoid
        // going through the global selection.
        if (!UfeSelectionUndoItem::select(
                "Pre-proxyAccessor selection", *Ufe::GlobalSelection::get())) {
            TF_WARN("Cannot save the selection.");
            return PullImportPaths();
        }

        // The "child" is the node that will receive the computed parent
        // transformation, in its offsetParentMatrix attribute.  We are using
        // the pull parent for this purpose, so pop the path of the ufeChild to
        // get to its pull parent.
        auto ufeChild = MayaUsd::ufe::dagPathToUfe(addedDagPath).pop();

        // Since we haven't pulled yet, obtaining the parent is simple, and
        // doesn't require going through the Hierarchy interface, which can do
        // non-trivial work on pulled objects to get their parent.
        auto ufeParent = ufePulledPath.pop();

        MString pyCommand;
        pyCommand.format(
            "from mayaUsd.lib import proxyAccessor as pa\n"
            "import maya.cmds as cmds\n"
            "cmds.select('^1s', '^2s')\n"
            "pa.parent()\n"
            "cmds.select(clear=True)\n",
            Ufe::PathString::string(ufeChild).c_str(),
            Ufe::PathString::string(ufeParent).c_str());

        MString pyUndoCommand;
        pyUndoCommand.format(
            "from mayaUsd.lib import proxyAccessor as pa\n"
            "import maya.cmds as cmds\n"
            "cmds.select('^1s', '^2s')\n"
            "pa.unparent()\n"
            "cmds.select(clear=True)\n",
            Ufe::PathString::string(ufeChild).c_str(),
            Ufe::PathString::string(ufeParent).c_str());

        if (!PythonUndoItem::execute(
                "Pull import proxy accessor parenting", pyCommand, pyUndoCommand)) {
            TF_WARN("Cannot parent pulled object.");
            return PullImportPaths();
        }

        // Create the pull set if it does not exists.
        //
        // Note: do not use the MFnSet API to create it as it clears the redo stack
        // and thus prevents redo.
        MObject pullSetObj;
        MStatus status = UsdMayaUtil::GetMObjectByName(kPullSetName, pullSetObj);
        if (status != MStatus::kSuccess) {
            MString createSetCmd;
            createSetCmd.format("sets -em -name \"^1s\";lockNode \"^1s\";", kPullSetName.asChar());
            MDGModifier& dgMod
                = MDGModifierUndoItem::create("Pull import pull set creation and lock");
            dgMod.commandToExecute(createSetCmd);
            dgMod.doIt();
        }

        // Finalize the pull.
        if (!FunctionUndoItem::execute(
                "Pull import pull info writing",
                [ufePulledPath, addedDagPath]() {
                    return writePullInformation(ufePulledPath, addedDagPath);
                },
                [ufePulledPath]() {
                    removePullInformation(ufePulledPath);
                    return true;
                })) {
            TF_WARN("Cannot write pull information metadata.");
            return PullImportPaths();
        }

        if (!FunctionUndoItem::execute(
                "Pull import rendering exclusion",
                [ufePulledPath]() { return addExcludeFromRendering(ufePulledPath); },
                [ufePulledPath]() {
                    removeExcludeFromRendering(ufePulledPath);
                    return true;
                })) {
            TF_WARN("Cannot exclude original USD data from viewport rendering.");
            return PullImportPaths();
        }

        if (!UfeSelectionUndoItem::select("Pull import select DAG node", addedDagPath)) {
            TF_WARN("Cannot select the pulled nodes.");
            return PullImportPaths();
        }
    }

    // Invert the new node registry, for MObject to Ufe::Path lookup.
    using ObjToUfePath = std::unordered_map<MObjectHandle, Ufe::Path>;
    ObjToUfePath objToUfePath;
    const auto&  ps = ufePulledPath.getSegments()[0];
    const auto   rtid = MayaUsd::ufe::getUsdRunTimeId();
    for (const auto& v : readJob->GetNewNodeRegistry()) {
        Ufe::Path::Segments s { ps, Ufe::PathSegment(v.first, rtid, '/') };
        Ufe::Path           p(std::move(s));
        objToUfePath.insert(ObjToUfePath::value_type(MObjectHandle(v.second), p));
    }

    // For each added Dag path, get the UFE path of the pulled USD prim.
    pulledUfePaths.reserve(addedDagPaths.size());
    for (const auto& dagPath : addedDagPaths) {
        auto found = objToUfePath.find(MObjectHandle(dagPath.node()));
        TF_AXIOM(found != objToUfePath.end());
        pulledUfePaths.emplace_back(found->second);
    }

    return PullImportPaths(addedDagPaths, pulledUfePaths);
}

//------------------------------------------------------------------------------
//
UsdMayaPrimUpdaterRegistry::RegisterItem getUpdaterItem(const MFnDependencyNode& dgNodeFn)
{
    MPlug usdTypeNamePlug = dgNodeFn.findPlug("USD_typeName", true);

    // If the Maya node holds USD type information (e.g. a dummy transform
    // node which is a stand-in for a non-transform USD prim type), use that
    // USD type intead of the Maya node type name.
    if (!usdTypeNamePlug.isNull())
        return UsdMayaPrimUpdaterRegistry::FindOrFallback(
            TfToken(usdTypeNamePlug.asString().asChar()));

    // In the absence of explicit USD type name, use the Maya type name.
    return UsdMayaPrimUpdaterRegistry::FindOrFallback(dgNodeFn.typeName().asChar());
}

//------------------------------------------------------------------------------
//
// Perform the customization step of the pull (second step).
bool pullCustomize(const PullImportPaths& importedPaths, const UsdMayaPrimUpdaterContext& context)
{
    // Record all USD modifications in an undo block and item.
    UsdUndoBlock undoBlock(
        &UsdUndoableItemUndoItem::create("Pull customize USD data modifications"));

    TF_AXIOM(importedPaths.first.size() == importedPaths.second.size());
    auto dagPathIt = importedPaths.first.begin();
    auto ufePathIt = importedPaths.second.begin();
    for (; dagPathIt != importedPaths.first.end(); ++dagPathIt, ++ufePathIt) {
        const auto&       dagPath = *dagPathIt;
        const auto&       pulledUfePath = *ufePathIt;
        MFnDependencyNode dgNodeFn(dagPath.node());

        auto registryItem = getUpdaterItem(dgNodeFn);
        auto factory = std::get<UsdMayaPrimUpdaterRegistry::UpdaterFactoryFn>(registryItem);
        auto updater = factory(context, dgNodeFn, pulledUfePath);

        // The failure of a single updater causes failure of the whole
        // customization step.  This is a frequent difficulty for operations on
        // multiple data, especially since we can't roll back the result of
        // the execution of previous updaters.  Revisit this.  PPT, 15-Sep-2021.
        if (!updater->editAsMaya()) {
            return false;
        }
    }
    return true;
}

//------------------------------------------------------------------------------
//
// Perform the export step of the merge to USD (first step).  Returns the
// source SdfPath and SdfLayer for the next step, push customize.  The source
// SdfPath will be empty on error.
using UsdPathToDagPathMap = TfHashMap<SdfPath, MDagPath, SdfPath::Hash>;
using UsdPathToDagPathMapPtr = std::shared_ptr<UsdPathToDagPathMap>;
using PushCustomizeSrc
    = std::tuple<SdfPath, UsdStageRefPtr, SdfLayerRefPtr, UsdPathToDagPathMapPtr>;

PushCustomizeSrc pushExport(
    const Ufe::Path&                 ufePulledPath,
    const MObject&                   mayaObject,
    const UsdMayaPrimUpdaterContext& context)
{
    UsdStageRefPtr         srcStage = UsdStage::CreateInMemory();
    SdfLayerRefPtr         srcLayer = srcStage->GetRootLayer();
    UsdPathToDagPathMapPtr pathMapPtr;
    auto pushCustomizeSrc = std::make_tuple(SdfPath(), srcStage, srcLayer, pathMapPtr);

    // Copy to be able to add the export root.
    VtDictionary userArgs = context.GetUserArgs();

    std::string fileName = srcLayer->GetIdentifier();

    MFnDagNode fnDag(mayaObject);
    MDagPath   dagPath;
    fnDag.getPath(dagPath);

    UsdMayaUtil::MDagPathSet dagPaths;
    dagPaths.insert(dagPath);

    std::vector<double> timeSamples;
    UsdMayaJobExportArgs::GetDictionaryTimeSamples(userArgs, timeSamples);

    // The pushed Dag node is the root of the export job.
    std::vector<VtValue> rootPathString(
        1, VtValue(std::string(dagPath.partialPathName().asChar())));
    userArgs[UsdMayaJobExportArgsTokens->exportRoots] = rootPathString;

    UsdMayaJobExportArgs jobArgs
        = UsdMayaJobExportArgs::CreateFromDictionary(userArgs, dagPaths, timeSamples);

    UsdMaya_WriteJob writeJob(jobArgs);
    if (!writeJob.Write(fileName, false /* append */)) {
        return pushCustomizeSrc;
    }

    std::get<SdfPath>(pushCustomizeSrc) = writeJob.MapDagPathToSdfPath(dagPath);

    // Invert the Dag path to USD path map, to return it for prim updater use.
    auto usdPathToDagPathMap = std::make_shared<UsdPathToDagPathMap>();
    for (const auto& v : writeJob.GetDagPathToUsdPathMap()) {
        usdPathToDagPathMap->insert(UsdPathToDagPathMap::value_type(v.second, v.first));
    }

    std::get<UsdPathToDagPathMapPtr>(pushCustomizeSrc) = usdPathToDagPathMap;

    return pushCustomizeSrc;
}

//------------------------------------------------------------------------------
//
SdfPath getDstSdfPath(const Ufe::Path& ufePulledPath, const SdfPath& srcSdfPath, bool isCopy)
{
    // If we got the destination path, extract it, otherwise use src path as
    // the destination.
    SdfPath dstSdfPath;
    if (ufePulledPath.nbSegments() == 2) {
        dstSdfPath = SdfPath(ufePulledPath.getSegments()[1].string());

        if (isCopy) {
            SdfPath relativeSrcSdfPath = srcSdfPath.MakeRelativePath(SdfPath::AbsoluteRootPath());
            dstSdfPath = dstSdfPath.AppendPath(relativeSrcSdfPath);
        }
    } else {
        dstSdfPath = srcSdfPath;
    }

    return dstSdfPath;
}

//------------------------------------------------------------------------------
//
// Create an updater for use with both pushCustomize() traversals /
// customization points: pushCopySpec() and pushEnd().
//
// pushCopySpec() and pushEnd() must use the same updater type.  An earlier
// version of this function tried to ensure this by using the pulled prim to
// create the updater.  However, this prim cannot be relied on, as
// pushCopySpec() has an edit router customization point that can remove the
// pulled prim from the USD scene (e.g. by switching a variant set to a
// different variant, such as what occurs when caching to a variant).  It is
// more robust to use the USD primSpec type at srcPath, which is in the
// srcLayer in the temporary stage.  If USD type round-tripping is set up
// properly (see UsdMayaTranslatorUtil::CreateDummyTransformNode()), this
// primSpec will have the type of the original pulled prim.
//
UsdMayaPrimUpdaterSharedPtr createUpdater(
    const SdfLayerRefPtr&            srcLayer,
    const SdfPath&                   srcPath,
    const SdfPath&                   dstPath,
    const UsdMayaPrimUpdaterContext& context)
{
    auto primSpec = srcLayer->GetPrimAtPath(srcPath);
    if (!TF_VERIFY(primSpec)) {
        return nullptr;
    }

    auto typeName = primSpec->GetTypeName();
    auto regItem = UsdMayaPrimUpdaterRegistry::FindOrFallback(typeName);
    auto factory = std::get<UpdaterFactoryFn>(regItem);

    // We cannot use the srcPath to create the UFE path, as this path is in the
    // in-memory stage in the temporary srcLayer and does not exist in UFE.
    // Use the dstPath instead, which can be validly added to the proxy shape
    // path to form a proper UFE path.
    auto                psPath = MayaUsd::ufe::stagePath(context.GetUsdStage());
    Ufe::Path::Segments segments { psPath.getSegments()[0],
                                   MayaUsd::ufe::usdPathToUfePathSegment(dstPath) };
    Ufe::Path           ufePath(std::move(segments));

    // Get the Maya object corresponding to the SdfPath.  As of 19-Oct-2021,
    // the export write job only registers Maya Dag path to SdfPath
    // correspondence, so prims that correspond to Maya DG nodes (e.g. material
    // networks) don't have a corresponding Dag path.  The prim updater
    // receives a null MObject in this case.
    auto              mayaDagPath = context.MapSdfPathToDagPath(srcPath);
    MFnDependencyNode depNodeFn(mayaDagPath.isValid() ? mayaDagPath.node() : MObject());

    return factory(context, depNodeFn, ufePath);
}

//------------------------------------------------------------------------------
//
// Perform the customization step of the merge to USD (second step).  Traverse
// the in-memory layer, creating a prim updater for each prim, and call Push
// for each updater.
bool pushCustomize(
    const Ufe::Path&                 ufePulledPath,
    const PushCustomizeSrc&          src,
    const UsdMayaPrimUpdaterContext& context)

{
    const auto& srcRootPath = std::get<SdfPath>(src);
    const auto& srcLayer = std::get<SdfLayerRefPtr>(src);
    const auto& srcStage = std::get<UsdStageRefPtr>(src);
    if (srcRootPath.IsEmpty() || !srcLayer || !srcStage) {
        return false;
    }

    const bool  isCopy = context.GetArgs()._copyOperation;
    const auto& editTarget = context.GetUsdStage()->GetEditTarget();
    auto dstRootPath = editTarget.MapToSpecPath(getDstSdfPath(ufePulledPath, srcRootPath, isCopy));
    auto dstRootParentPath = dstRootPath.GetParentPath();
    const auto& dstLayer = editTarget.GetLayer();

    // Traverse the layer, creating a prim updater for each primSpec
    // along the way, and call PushCopySpec on the prim.
    auto pushCopySpecsFn
        = [&context, srcStage, srcLayer, dstLayer, dstRootParentPath](const SdfPath& srcPath) {
              // We can be called with a primSpec path that is not a prim path
              // (e.g. a property path like "/A.xformOp:translate").  This is not an
              // error, just prune the traversal.  FIXME Is this still true?  We
              // should not be traversing property specs.  PPT, 20-Oct-2021.
              if (!srcPath.IsPrimPath()) {
                  return false;
              }

              auto dstPath = makeDstPath(dstRootParentPath, srcPath);
              auto updater = createUpdater(srcLayer, srcPath, dstPath, context);
              // If we cannot find an updater for the srcPath, prune the traversal.
              if (!updater) {
                  TF_WARN(
                      "Could not create a prim updater for path %s during PushCopySpecs traversal, "
                      "pruning at that point.",
                      srcPath.GetText());
                  return false;
              }

              // Report PushCopySpecs() failure.
              auto result = updater->pushCopySpecs(
                  srcStage, srcLayer, srcPath, context.GetUsdStage(), dstLayer, dstPath);
              if (result == UsdMayaPrimUpdater::PushCopySpecs::Failed) {
                  throw MayaUsd::TraversalFailure(std::string("PushCopySpecs() failed."), srcPath);
              }

              // If we don't continue, we prune.
              return result == UsdMayaPrimUpdater::PushCopySpecs::Continue;
          };

    if (!MayaUsd::traverseLayer(srcLayer, srcRootPath, pushCopySpecsFn)) {
        return false;
    }

    // Push end is a separate traversal, not a second phase of the same
    // traversal, because it is post-order: parents are traversed after
    // children.  This allows for proper parent lifescope, if push end
    // deletes the Maya node (which is the default behavior).
    if (isCopy) {
        return true;
    }

    // SdfLayer::TraversalFn does not return a status, so must report
    // failure through an exception.
    auto pushEndFn = [&context, srcLayer, dstLayer, dstRootParentPath](const SdfPath& srcPath) {
        // We can be called with a primSpec path that is not a prim path
        // (e.g. a property path like "/A.xformOp:translate").  This is not an
        // error, just a no-op.
        if (!srcPath.IsPrimPath()) {
            return;
        }

        auto dstPath = makeDstPath(dstRootParentPath, srcPath);
        auto updater = createUpdater(srcLayer, srcPath, dstPath, context);
        if (!updater) {
            TF_WARN(
                "Could not create a prim updater for path %s during PushEnd() traversal, pruning "
                "at that point.",
                srcPath.GetText());
            return;
        }

        // Report pushEnd() failure.
        if (!updater->pushEnd()) {
            throw MayaUsd::TraversalFailure(std::string("PushEnd() failed."), srcPath);
        }
    };

    // SdfLayer::Traverse does not return a status, so must report failure
    // through an exception.
    try {
        srcLayer->Traverse(srcRootPath, pushEndFn);
    } catch (const MayaUsd::TraversalFailure& e) {
        TF_WARN(
            "PushEnd() layer traversal failed for path %s: %s",
            e.path().GetText(),
            e.reason().c_str());
        return false;
    }

    return true;
}

class PushPullScope
{
public:
    PushPullScope(bool& controlingFlag)
    {
        if (!controlingFlag) {
            controlingFlag = true;
            _controlingFlag = &controlingFlag;
        }
    }
    ~PushPullScope()
    {
        if (_controlingFlag) {
            *_controlingFlag = false;
        }
    }

private:
    bool* _controlingFlag { nullptr };
};

} // namespace

PXR_NAMESPACE_OPEN_SCOPE

TF_INSTANTIATE_SINGLETON(PrimUpdaterManager);

PrimUpdaterManager::PrimUpdaterManager()
{
    TfSingleton<PrimUpdaterManager>::SetInstanceConstructed(*this);
    TfRegistryManager::GetInstance().SubscribeTo<PrimUpdaterManager>();

    TfWeakPtr<PrimUpdaterManager> me(this);
    TfNotice::Register(me, &PrimUpdaterManager::onProxyContentChanged);
}

PrimUpdaterManager::~PrimUpdaterManager() { }

bool PrimUpdaterManager::mergeToUsd(
    const MFnDependencyNode& depNodeFn,
    const Ufe::Path&         pulledPath,
    const VtDictionary&      userArgs)
{
    MayaUsdProxyShapeBase* proxyShape = MayaUsd::ufe::getProxyShape(pulledPath);
    if (!proxyShape) {
        return false;
    }

    auto pulledPrim = MayaUsd::ufe::ufePathToPrim(pulledPath);
    if (!pulledPrim) {
        return false;
    }

    PushPullScope scopeIt(_inPushPull);

    auto ctxArgs = VtDictionaryOver(userArgs, UsdMayaJobExportArgs::GetDefaultDictionary());

    auto       updaterArgs = UsdMayaPrimUpdaterArgs::createFromDictionary(ctxArgs);
    auto       mayaPath = usdToMaya(pulledPath);
    auto       mayaDagPath = MayaUsd::ufe::ufeToDagPath(mayaPath);
    MDagPath   pullParentPath;
    const bool isCopy = updaterArgs._copyOperation;
    if (!isCopy) {
        // The pull parent is simply the parent of the pulled path.
        pullParentPath = MayaUsd::ufe::ufeToDagPath(mayaPath.pop());
        if (!TF_VERIFY(pullParentPath.isValid())) {
            return false;
        }
        if (!LockNodesUndoItem::lock("Merge to USD node unlocking", pullParentPath, false)) {
            return false;
        }
    }

    // If the user-provided argument does *not* contain an animation key, then
    // automatically infer if we should merge animations.
    if (!VtDictionaryIsHolding<bool>(userArgs, UsdMayaJobExportArgsTokens->animation)) {
        const bool isAnimated = PXR_NS::UsdMayaPrimUpdater::isAnimated(mayaDagPath);
        GfInterval timeInterval = isAnimated
            ? GfInterval(MAnimControl::minTime().value(), MAnimControl::maxTime().value())
            : GfInterval();

        ctxArgs[UsdMayaJobExportArgsTokens->animation] = isAnimated;
        ctxArgs[UsdMayaJobExportArgsTokens->frameStride] = 1.0;
        ctxArgs[UsdMayaJobExportArgsTokens->startTime] = timeInterval.GetMin();
        ctxArgs[UsdMayaJobExportArgsTokens->endTime] = timeInterval.GetMax();
    }

    // Reset the selection, otherwise it will keep a reference to a deleted node
    // and crash later on.
    if (!UfeSelectionUndoItem::clear("Merge to USD selection reset")) {
        TF_WARN("Cannot reset the selection.");
        return false;
    }

    UsdStageRefPtr            proxyStage = proxyShape->usdPrim().GetStage();
    UsdMayaPrimUpdaterContext context(proxyShape->getTime(), proxyStage, ctxArgs);

    auto  ufeMayaItem = Ufe::Hierarchy::createItem(mayaPath);
    auto& scene = Ufe::Scene::instance();
    if (!isCopy && TF_VERIFY(ufeMayaItem))
        scene.notify(Ufe::ObjectPreDelete(ufeMayaItem));

    // Record all USD modifications in an undo block and item.
    UsdUndoBlock undoBlock(
        &UsdUndoableItemUndoItem::create("Merge to Maya USD data modifications"));

    // The push is done in two stages:
    // 1) Perform the export into a temporary layer.
    // 2) Traverse the layer and call the prim updater for each prim, for
    //    per-prim customization.

    // 1) Perform the export to the temporary layer.
    auto pushCustomizeSrc = pushExport(pulledPath, depNodeFn.object(), context);

    // 2) Traverse the in-memory layer, creating a prim updater for each prim,
    // and call Push for each updater.  Build a new context with the USD path
    // to Maya path mapping information.
    UsdMayaPrimUpdaterContext customizeContext(
        proxyShape->getTime(),
        proxyStage,
        ctxArgs,
        std::get<UsdPathToDagPathMapPtr>(pushCustomizeSrc));

    if (!isCopy) {
        if (!FunctionUndoItem::execute(
                "Merge to Maya rendering inclusion",
                [pulledPath]() {
                    removeExcludeFromRendering(pulledPath);
                    return true;
                },
                [pulledPath]() { return addExcludeFromRendering(pulledPath); })) {
            TF_WARN("Cannot re-enable original USD data in viewport rendering.");
            return false;
        }
    }

    if (!pushCustomize(pulledPath, pushCustomizeSrc, customizeContext)) {
        return false;
    }

    if (!isCopy) {
        if (!FunctionUndoItem::execute(
                "Merge to Maya pull info removal",
                [pulledPath]() {
                    removePullInformation(pulledPath);
                    return true;
                },
                [pulledPath, mayaDagPath]() {
                    return writePullInformation(pulledPath, mayaDagPath);
                })) {
            TF_WARN("Cannot remove pull information metadata.");
            return false;
        }
    }

    // Discard all pulled Maya nodes.
    std::vector<MDagPath> toApplyOn = UsdMayaUtil::getDescendantsStartingWithChildren(mayaDagPath);
    for (const MDagPath& curDagPath : toApplyOn) {
        MStatus status = NodeDeletionUndoItem::deleteNode(
            "Merge to USD Maya scene cleanup", curDagPath.fullPathName(), curDagPath.node());
        if (status != MS::kSuccess) {
            TF_WARN(
                "Merge to USD Maya scene cleanup: cannot delete node \"%s\".",
                curDagPath.fullPathName().asChar());
            return false;
        }
    }

    if (!isCopy) {
        if (!TF_VERIFY(removePullParent(pullParentPath))) {
            return false;
        }
    }

    // Some updaters (like MayaReference) may be writing and changing the variant during merge.
    // This will change the hierarchy around pulled prim. Grab hierarchy from the parent.
    auto ufeUsdItem = Ufe::Hierarchy::createItem(pulledPath.pop());
    auto hier = Ufe::Hierarchy::hierarchy(ufeUsdItem);
    if (TF_VERIFY(hier)) {
        scene.notify(Ufe::SubtreeInvalidate(hier->defaultParent()));
    }

    return true;
}

bool PrimUpdaterManager::editAsMaya(const Ufe::Path& path, const VtDictionary& userArgs)
{
    if (hasEditedDescendant(path)) {
        TF_WARN("Cannot edit an ancestor of an already edited node.");
        return false;
    }

    MayaUsdProxyShapeBase* proxyShape = MayaUsd::ufe::getProxyShape(path);
    if (!proxyShape) {
        return false;
    }

    auto pulledPrim = MayaUsd::ufe::ufePathToPrim(path);
    if (!pulledPrim) {
        return false;
    }

    PushPullScope scopeIt(_inPushPull);

    auto ctxArgs = VtDictionaryOver(userArgs, UsdMayaJobImportArgs::GetDefaultDictionary());
    auto updaterArgs = UsdMayaPrimUpdaterArgs::createFromDictionary(ctxArgs);

    MDagPath pullParentPath;
    if (!updaterArgs._copyOperation
        && !(pullParentPath = setupPullParent(path, ctxArgs)).isValid()) {
        TF_WARN("Cannot setup the edit parent node.");
        return false;
    }

    UsdMayaPrimUpdaterContext context(proxyShape->getTime(), pulledPrim.GetStage(), ctxArgs);

    auto& scene = Ufe::Scene::instance();
    auto  ufeItem = Ufe::Hierarchy::createItem(path);
    if (!updaterArgs._copyOperation && TF_VERIFY(ufeItem))
        scene.notify(Ufe::ObjectPreDelete(ufeItem));

    // The pull is done in two stages:
    // 1) Perform the import into Maya.
    // 2) Iterate over all imported Dag paths and call the prim updater on
    //    each, for per-prim customization.

    // 1) Perform the import
    PullImportPaths importedPaths = pullImport(path, pulledPrim, context);
    if (importedPaths.first.empty()) {
        return false;
    }

    // 2) Iterate over all imported Dag paths.
    if (!pullCustomize(importedPaths, context)) {
        TF_WARN("Failed to customize the edited nodes.");
        return false;
    }

    if (!updaterArgs._copyOperation) {
        // Lock pulled nodes starting at the pull parent.
        if (!LockNodesUndoItem::lock("Edit as Maya node locking", pullParentPath, true))
            return false;

        // Allow editing topology, which gets turned of by locking.
        if (!allowTopologyModifications(pullParentPath))
            return false;
    }

    // We must recreate the UFE item because it has changed data models (USD -> Maya).
    ufeItem = Ufe::Hierarchy::createItem(usdToMaya(path));
    if (TF_VERIFY(ufeItem))
        scene.notify(Ufe::ObjectAdd(ufeItem));

    return true;
}

bool PrimUpdaterManager::canEditAsMaya(const Ufe::Path& path) const
{
    // Verify if the prim is an ancestor of an edited prim.
    if (hasEditedDescendant(path))
        return false;

    // Create a prim updater for the path, and ask it if the prim can be edited
    // as Maya.
    auto prim = MayaUsd::ufe::ufePathToPrim(path);
    if (!prim) {
        return false;
    }

    VtDictionary              userArgs;
    UsdMayaPrimUpdaterContext context(UsdTimeCode::Default(), prim.GetStage(), userArgs);

    auto typeName = prim.GetTypeName();
    auto regItem = UsdMayaPrimUpdaterRegistry::FindOrFallback(typeName);
    auto factory = std::get<UpdaterFactoryFn>(regItem);
    // No Maya Dag path for the prim updater, so pass in a null MObject.
    auto updater = factory(context, MFnDependencyNode(MObject()), path);
    return updater ? updater->canEditAsMaya() : false;
}

bool PrimUpdaterManager::discardEdits(const MDagPath& dagPath)
{
    Ufe::Path primPath;
    if (!PXR_NS::PrimUpdaterManager::readPullInformation(dagPath, primPath))
        return false;

    auto usdPrim = MayaUsd::ufe::ufePathToPrim(primPath);
    return usdPrim ? discardPrimEdits(primPath) : discardOrphanedEdits(dagPath);
}

bool PrimUpdaterManager::discardPrimEdits(const Ufe::Path& pulledPath)
{
    MayaUsdProxyShapeBase* proxyShape = MayaUsd::ufe::getProxyShape(pulledPath);
    if (!proxyShape) {
        return false;
    }

    PushPullScope scopeIt(_inPushPull);

    // Record all USD modifications in an undo block and item.
    UsdUndoBlock undoBlock(
        &UsdUndoableItemUndoItem::create("Discard edits USD data modifications"));

    auto mayaPath = usdToMaya(pulledPath);
    auto mayaDagPath = MayaUsd::ufe::ufeToDagPath(mayaPath);

    UsdMayaPrimUpdaterContext context(
        proxyShape->getTime(), proxyShape->usdPrim().GetStage(), VtDictionary());

    auto  ufeMayaItem = Ufe::Hierarchy::createItem(mayaPath);
    auto& scene = Ufe::Scene::instance();
    if (TF_VERIFY(ufeMayaItem))
        scene.notify(Ufe::ObjectPreDelete(ufeMayaItem));

    // Unlock the pulled hierarchy, clear the pull information, and remove the
    // pull parent, which is simply the parent of the pulled path.
    auto pullParent = mayaDagPath;
    pullParent.pop();
    if (!TF_VERIFY(pullParent.isValid())) {
        return false;
    }
    if (!LockNodesUndoItem::lock("Discard edits node unlocking", pullParent, false)) {
        return false;
    }

    // Reset the selection, otherwise it will keep a reference to a deleted node
    // and crash later on.
    if (!UfeSelectionUndoItem::clear("Discard edits selection reset")) {
        TF_WARN("Cannot reset the selection.");
        return false;
    }

    // Discard all pulled Maya nodes.
    std::vector<MDagPath> toApplyOn = UsdMayaUtil::getDescendantsStartingWithChildren(mayaDagPath);
    for (const MDagPath& curDagPath : toApplyOn) {
        MFnDependencyNode dgNodeFn(curDagPath.node());

        const Ufe::Path path = MayaUsd::ufe::dagPathToPathSegment(curDagPath);

        auto registryItem = getUpdaterItem(dgNodeFn);
        auto factory = std::get<UsdMayaPrimUpdaterRegistry::UpdaterFactoryFn>(registryItem);
        auto updater = factory(context, dgNodeFn, path);

        updater->discardEdits();
    }

    if (!FunctionUndoItem::execute(
            "Discard edits pull info removal",
            [pulledPath]() {
                removePullInformation(pulledPath);
                return true;
            },
            [pulledPath, mayaDagPath]() {
                return writePullInformation(pulledPath, mayaDagPath);
            })) {
        TF_WARN("Cannot remove pull information metadata.");
        return false;
    }

    if (!FunctionUndoItem::execute(
            "Discard edits rendering inclusion",
            [pulledPath]() {
                removeExcludeFromRendering(pulledPath);
                return true;
            },
            [pulledPath]() { return addExcludeFromRendering(pulledPath); })) {
        TF_WARN("Cannot re-enable original USD data in viewport rendering.");
        return false;
    }

    if (!TF_VERIFY(removePullParent(pullParent))) {
        return false;
    }

    auto ufeUsdItem = Ufe::Hierarchy::createItem(pulledPath);
    auto hier = Ufe::Hierarchy::hierarchy(ufeUsdItem);
    if (TF_VERIFY(hier)) {
        scene.notify(Ufe::SubtreeInvalidate(hier->defaultParent()));
    }
    return true;
}

bool PrimUpdaterManager::discardOrphanedEdits(const MDagPath& dagPath)
{
    PushPullScope scopeIt(_inPushPull);

    // Unlock the pulled hierarchy, clear the pull information, and remove the
    // pull parent, which is simply the parent of the pulled path.
    auto pullParent = dagPath;
    pullParent.pop();

    if (!LockNodesUndoItem::lock("Discard orphaned edits node unlocking", pullParent, false)) {
        return false;
    }

    // Reset the selection, otherwise it will keep a reference to a deleted node
    // and crash later on.
    if (!UfeSelectionUndoItem::clear("Discard orphaned edits selection reset")) {
        TF_WARN("Cannot reset the selection.");
        return false;
    }

    UsdMayaPrimUpdaterContext context(UsdTimeCode(), nullptr, VtDictionary());

    // Discard all pulled Maya nodes.
    std::vector<MDagPath> toApplyOn = UsdMayaUtil::getDescendantsStartingWithChildren(dagPath);
    for (const MDagPath& curDagPath : toApplyOn) {
        MFnDependencyNode dgNodeFn(curDagPath.node());

        auto registryItem = getUpdaterItem(dgNodeFn);
        auto factory = std::get<UsdMayaPrimUpdaterRegistry::UpdaterFactoryFn>(registryItem);
        auto updater = factory(context, dgNodeFn, Ufe::Path());

        updater->discardEdits();
    }

    if (!TF_VERIFY(removePullParent(pullParent))) {
        return false;
    }

    return true;
}

bool PrimUpdaterManager::duplicate(
    const Ufe::Path&    srcPath,
    const Ufe::Path&    dstPath,
    const VtDictionary& userArgs)
{
    MayaUsdProxyShapeBase* srcProxyShape = MayaUsd::ufe::getProxyShape(srcPath);
    MayaUsdProxyShapeBase* dstProxyShape = MayaUsd::ufe::getProxyShape(dstPath);

    PushPullScope scopeIt(_inPushPull);

    // Copy from USD to DG
    if (srcProxyShape && dstProxyShape == nullptr) {
        auto srcPrim = MayaUsd::ufe::ufePathToPrim(srcPath);
        if (!srcPrim) {
            return false;
        }

        auto ctxArgs = VtDictionaryOver(userArgs, UsdMayaJobImportArgs::GetDefaultDictionary());

        // We will only do copy between two data models, setting this in arguments
        // to configure the updater
        ctxArgs[UsdMayaPrimUpdaterArgsTokens->copyOperation] = true;

        // Set destination of duplicate. The Maya world MDagPath is not valid,
        // so don't try to validate the path if it is the world root.
        MDagPath pullParentPath;
        if (!MayaUsd::ufe::isMayaWorldPath(dstPath)) {
            pullParentPath = MayaUsd::ufe::ufeToDagPath(dstPath);
            if (!pullParentPath.isValid()) {
                return false;
            }
        }
        ctxArgs[kPullParentPathKey] = VtValue(std::string(pullParentPath.fullPathName().asChar()));

        UsdMayaPrimUpdaterContext context(
            srcProxyShape->getTime(), srcProxyShape->getUsdStage(), ctxArgs);

        pullImport(srcPath, srcPrim, context);
        return true;
    }
    // Copy from DG to USD
    else if (srcProxyShape == nullptr && dstProxyShape) {
        TF_AXIOM(srcPath.nbSegments() == 1);
        MDagPath dagPath = PXR_NS::UsdMayaUtil::nameToDagPath(Ufe::PathString::string(srcPath));
        if (!dagPath.isValid()) {
            return false;
        }

        auto ctxArgs = VtDictionaryOver(userArgs, UsdMayaJobExportArgs::GetDefaultDictionary());

        // Record all USD modifications in an undo block and item.
        MAYAUSD_NS::UsdUndoBlock undoBlock(
            &UsdUndoableItemUndoItem::create("Duplicate USD data modifications"));

        // We will only do copy between two data models, setting this in arguments
        // to configure the updater
        ctxArgs[UsdMayaPrimUpdaterArgsTokens->copyOperation] = true;
        auto                      dstStage = dstProxyShape->getUsdStage();
        UsdMayaPrimUpdaterContext context(dstProxyShape->getTime(), dstStage, ctxArgs);

        // Export out to a temporary layer.
        auto        pushExportOutput = pushExport(srcPath, dagPath.node(), context);
        const auto& srcRootPath = std::get<SdfPath>(pushExportOutput);
        if (srcRootPath.IsEmpty()) {
            return false;
        }

        // Copy the temporary layer contents out to the proper destination.
        const auto& srcLayer = std::get<SdfLayerRefPtr>(pushExportOutput);
        const auto& editTarget = dstStage->GetEditTarget();
        const auto& dstLayer = editTarget.GetLayer();

        // Validate that the destination parent prim is valid.
        UsdPrim dstParentPrim = MayaUsd::ufe::ufePathToPrim(dstPath);
        if (!dstParentPrim.IsValid()) {
            return false;
        }

        // Make the destination root path unique.
        SdfPath     dstParentPath = dstParentPrim.GetPath();
        std::string dstChildName = ufe::uniqueChildName(dstParentPrim, srcRootPath.GetName());
        SdfPath     dstRootPath = dstParentPath.AppendChild(TfToken(dstChildName));

        if (!SdfCopySpec(srcLayer, srcRootPath, dstLayer, dstRootPath)) {
            return false;
        }

        auto ufeItem = Ufe::Hierarchy::createItem(dstPath);
        if (TF_VERIFY(ufeItem)) {
            Ufe::Scene::instance().notify(Ufe::SubtreeInvalidate(ufeItem));
        }
        return true;
    }

    // Copy operations to the same data model not supported here.
    return false;
}

void PrimUpdaterManager::onProxyContentChanged(
    const MayaUsdProxyStageObjectsChangedNotice& proxyNotice)
{
    if (_inPushPull) {
        return;
    }

    auto proxyShapeUfePath = proxyNotice.GetProxyShape().ufePath();

    auto autoEditFn = [this, proxyShapeUfePath](
                          const UsdMayaPrimUpdaterContext& context, const UsdPrim& prim) -> bool {
        TfToken typeName = prim.GetTypeName();

        auto registryItem = UsdMayaPrimUpdaterRegistry::FindOrFallback(typeName);
        auto supports = std::get<UsdMayaPrimUpdater::Supports>(registryItem);

        if ((supports & UsdMayaPrimUpdater::Supports::AutoPull)
            != UsdMayaPrimUpdater::Supports::AutoPull)
            return false;

        const Ufe::PathSegment pathSegment = MayaUsd::ufe::usdPathToUfePathSegment(prim.GetPath());
        const Ufe::Path        path = proxyShapeUfePath + pathSegment;

        auto factory = std::get<UpdaterFactoryFn>(registryItem);
        auto updater = factory(context, MFnDependencyNode(MObject()), path);

        if (updater && updater->shouldAutoEdit()) {
            // TODO UNDO: is it okay to throw away the undo info in the change notification?
            // What could we do with it anyway?
            OpUndoItemMuting muting;
            this->editAsMaya(path);

            return true;
        }
        return false;
    };

    const UsdNotice::ObjectsChanged& notice = proxyNotice.GetNotice();

    Usd_PrimFlagsPredicate predicate = UsdPrimDefaultPredicate;

    auto stage = notice.GetStage();

    VtDictionary              userArgs;
    UsdMayaPrimUpdaterContext context(UsdTimeCode::Default(), stage, userArgs);

    for (const auto& changedPath : notice.GetResyncedPaths()) {
        UsdPrim resyncPrim = (changedPath != SdfPath::AbsoluteRootPath())
            ? stage->GetPrimAtPath(changedPath)
            : stage->GetPseudoRoot();

        UsdPrimRange range(resyncPrim, predicate);

        for (auto it = range.begin(); it != range.end(); it++) {
            const UsdPrim& prim = *it;
            if (autoEditFn(context, prim)) {
                it.PruneChildren();
            }
        }
    }

    auto changedInfoOnlyPaths = notice.GetChangedInfoOnlyPaths();
    for (auto it = changedInfoOnlyPaths.begin(), end = changedInfoOnlyPaths.end(); it != end;
         ++it) {
        const auto& changedPath = *it;
        if (changedPath.IsPrimPropertyPath()) {
            UsdPrim valueChangedPrim = stage->GetPrimAtPath(changedPath.GetPrimPath());
            if (valueChangedPrim) {
                autoEditFn(context, valueChangedPrim);
            }
        }
    }
}

PrimUpdaterManager& PrimUpdaterManager::getInstance()
{
    return TfSingleton<PrimUpdaterManager>::GetInstance();
}

MObject PrimUpdaterManager::findOrCreatePullRoot()
{
    MObject pullRoot = findPullRoot();
    if (!pullRoot.isNull()) {
        return pullRoot;
    }

    // No pull root in the scene, so create one.
    MDagModifier& dagMod = MDagModifierUndoItem::create("Create pull root");
    MStatus       status;
    MObject       pullRootObj = dagMod.createNode(MString("transform"), MObject::kNullObj, &status);
    if (status != MStatus::kSuccess) {
        return MObject();
    }
    status = dagMod.renameNode(pullRootObj, kPullRootName);
    if (status != MStatus::kSuccess) {
        return MObject();
    }

    if (dagMod.doIt() != MStatus::kSuccess) {
        return MObject();
    }

    // Hide all objects under the pull root in the Outliner so only the pulled
    // objects under a proxy shape will be shown.
    //
    // TODO UNDO: make this redoable? Pull is always redone from scratch for now, so it does not
    // look necessary.
    MFnDependencyNode pullRootFn(pullRootObj);
    UsdMayaUtil::SetHiddenInOutliner(pullRootFn, true);

    if (!FunctionUndoItem::execute(
            "Create pull root cache has pulled prims",
            [self = this]() {
                self->_hasPulledPrims = true;
                return true;
            },
            [self = this]() {
                self->_hasPulledPrims = false;
                return true;
            })) {
        TF_WARN("Cannot create pulled prim cache.");
        return MObject();
    }

    return pullRootObj;
}

MObject PrimUpdaterManager::createPullParent(const Ufe::Path& pulledPath, MObject pullRoot)
{
    MDagModifier& dagMod = MDagModifierUndoItem::create("Create pull parent node");
    MStatus       status;
    MObject       pullParentObj = dagMod.createNode(MString("transform"), pullRoot, &status);
    if (status != MStatus::kSuccess) {
        return MObject::kNullObj;
    }

    // Rename the pull parent to be the name of the node plus a "Parent" suffix.
    status = dagMod.renameNode(
        pullParentObj, MString(pulledPath.back().string().c_str()) + MString("Parent"));

    return (dagMod.doIt() == MStatus::kSuccess) ? pullParentObj : MObject::kNullObj;
}

bool PrimUpdaterManager::removePullParent(const MDagPath& parentDagPath)
{
    if (!TF_VERIFY(parentDagPath.isValid())) {
        return false;
    }

    MStatus status = NodeDeletionUndoItem::deleteNode(
        "Delete pull parent node", parentDagPath.fullPathName(), parentDagPath.node());
    if (status != MStatus::kSuccess)
        return false;

    // If the pull parent was the last child of the pull root, remove the pull
    // root as well, and null out our pull root cache.
    MObject pullRoot = findPullRoot();
    if (!pullRoot.isNull()) {
        MFnDagNode pullRootNode(pullRoot);
        auto       nbPullRootChildren = pullRootNode.childCount();
        if (nbPullRootChildren == 0) {
            status = NodeDeletionUndoItem::deleteNode(
                "Delete pull root", pullRootNode.absoluteName(), pullRoot);
            if (status != MStatus::kSuccess) {
                return false;
            }
            if (!FunctionUndoItem::execute(
                    "Delete pull root cache no pulled prims",
                    [self = this]() {
                        self->_hasPulledPrims = false;
                        return true;
                    },
                    [self = this]() {
                        self->_hasPulledPrims = true;
                        return true;
                    })) {
                TF_WARN("Cannot removed pulled prim from the pulled prim cache.");
                return false;
            }
        }
    }

    return true;
}

MDagPath PrimUpdaterManager::setupPullParent(const Ufe::Path& pulledPath, VtDictionary& args)
{
    // Record all USD modifications in an undo block and item.
    UsdUndoBlock undoBlock(
        &UsdUndoableItemUndoItem::create("Setup pull parent USD data modification"));

    MObject pullRoot = findOrCreatePullRoot();
    if (pullRoot.isNull()) {
        return MDagPath();
    }

    auto pullParent = createPullParent(pulledPath, pullRoot);
    if (pullParent == MObject::kNullObj) {
        return MDagPath();
    }

    // Pull parent is not instanced, so use first path found.
    MDagPath pullParentPath;
    if (MDagPath::getAPathTo(pullParent, pullParentPath) != MStatus::kSuccess) {
        return MDagPath();
    }

    // Add pull parent path to import args as a string.
    args[kPullParentPathKey] = VtValue(std::string(pullParentPath.fullPathName().asChar()));

    return pullParentPath;
}

/* static */
bool PrimUpdaterManager::readPullInformation(const PXR_NS::UsdPrim& prim, std::string& dagPathStr)
{
    auto value = prim.GetCustomDataByKey(kPullPrimMetadataKey);
    if (!value.IsEmpty() && value.CanCast<std::string>()) {
        dagPathStr = value.Get<std::string>();
        return !dagPathStr.empty();
    }
    return false;
}

/* static */
bool PrimUpdaterManager::readPullInformation(
    const PXR_NS::UsdPrim& prim,
    Ufe::SceneItem::Ptr&   dagPathItem)
{
    std::string dagPathStr;
    if (readPullInformation(prim, dagPathStr)) {
        dagPathItem = Ufe::Hierarchy::createItem(Ufe::PathString::path(dagPathStr));
        return (bool)dagPathItem;
    }
    return false;
}

/* static */
bool PrimUpdaterManager::readPullInformation(const Ufe::Path& ufePath, MDagPath& dagPath)
{
    auto        prim = MayaUsd::ufe::ufePathToPrim(ufePath);
    std::string dagPathStr;
    if (readPullInformation(prim, dagPathStr)) {
        MSelectionList sel;
        sel.add(dagPathStr.c_str());
        sel.getDagPath(0, dagPath);
        return dagPath.isValid();
    }
    return false;
}

/* static */
bool PrimUpdaterManager::readPullInformation(const MDagPath& dagPath, Ufe::Path& ufePath)
{
    MStatus status;

    MFnDependencyNode depNode(dagPath.node());
    MPlug             dgMetadata = depNode.findPlug(kPullDGMetadataKey, &status);
    if (status == MStatus::kSuccess) {
        MString pulledUfePathStr;
        status = dgMetadata.getValue(pulledUfePathStr);
        if (status) {
            ufePath = Ufe::PathString::path(pulledUfePathStr.asChar());
            return !ufePath.empty();
        }
    }

    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
