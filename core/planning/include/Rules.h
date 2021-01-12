#ifndef LIGHTDB_RULES_H
#define LIGHTDB_RULES_H

#include "Model.h"
#include "Optimizer.h"
#include "ScanOperators.h"
#include "EncodeOperators.h"
#include "DecodeOperators.h"
#include "UnionOperators.h"
#include "MapOperators.h"
#include "TransferOperators.h"
#include "DiscretizeOperators.h"
#include "InterpolateOperators.h"
#include "SubqueryOperators.h"
#include "IdentityOperators.h"
#include "HomomorphicOperators.h"
#include "SubsetOperators.h"
#include "StoreOperators.h"
#include "SaveOperators.h"
#include "SinkOperators.h"
#include "Rectangle.h"

#include <iostream>
#include "CrackingOperators.h"
#include "MultiTileOperators.h"
#include "SelectFramesOperators.h"
#include "SelectPixelsOperators.h"
#include "TileOperators.h"
#include "WorkloadCostEstimator.h"
#include "ContainsCar.h"
#include "extension.h"
#include "TasmOperators.h"
#include "SelectPixelsKernel.h"

namespace lightdb::optimization {
    class ChooseMaterializedScans : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const LightField &node) override {
            if (plan().has_physical_assignment(node))
                return false;
            else if(node.is<physical::GPUDecodedFrameData>()) {
                auto ref = plan().lookup(node);
                auto &m = node.downcast<physical::GPUDecodedFrameData>();
                auto mref = physical::MaterializedLightFieldReference::make<physical::GPUDecodedFrameData>(m);

                //Removed this line without actually testing the consequences
                //plan().emplace<physical::GPUScanMemory>(ref, mref);
                //plan().emplace<physical::GPUOperatorAdapter>(mref);
                // Made this change without testing it -- when is this rule fired?
                plan().emplace<physical::MaterializedToPhysicalOperatorAdapter>(ref, mref);
                return true;
            } else if(node.is<physical::PhysicalToLogicalLightFieldAdapter>()) {
                auto ref = plan().lookup(node);
                //auto op = plan().emplace<physical::GPUOperatorAdapter>(ref.downcast<physical::PhysicalToLogicalLightFieldAdapter>().source());
                //plan().assign(ref, op);
                plan().assign(ref, ref.downcast<physical::PhysicalToLogicalLightFieldAdapter>().source());
                return true;
            } else
                return false;
        }
    };

    class ChooseDecoders : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::TileGenerationConfigLightField &node) override {
            if (!plan().has_physical_assignment(node)) {

                plan().emplace<physical::GPUCreateBlackTile>(plan().lookup(node),
                                                            plan().allocator().gpu(),
                                                            node.codec(),
                                                            node.width(),
                                                            node.height(),
                                                            node.numFrames());
                return true;
            }
            return false;
        }

        bool visit(const logical::MultiTiledLightFieldForRetiling &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto framesToRetile = node.metadataManager()->orderedFramesForMetadata();
                auto locationProvider = std::make_shared<tiles::SingleTileLocationProvider>(node.tileLayoutsManager());

                int gopLength = node.entry()->sources()[0].configuration().framerate.fps();

                if (node.retileStrategy() == logical::RetileStrategy::RetileIfDifferent || node.retileStrategy() == logical::RetileStrategy::RetileAlways) {
                    auto configProvider = node.tileConfigurationProvider();

                    std::vector<int> framesWithDifferentLayout;
                    if (node.retileStrategy() == logical::RetileStrategy::RetileIfDifferent) {
                        // For each GOP, compare the number of pixels to decode with the current layout and the proposed layout.
                        Workload workload(node.metadataManager()->metadataIdentifier(),
                                          {node.metadataManager()->metadataSpecification()}, {1});
                        WorkloadCostEstimator currentLayoutEstimator(locationProvider, workload, gopLength, 1, 0, 0);
                        WorkloadCostEstimator proposedLayoutEstimator(configProvider, workload, gopLength, 1, 0, 0);

                        std::unique_ptr<std::unordered_map<unsigned int, CostElements>> currentCosts(
                                new std::unordered_map<unsigned int, CostElements>());
                        std::unique_ptr<std::unordered_map<unsigned int, CostElements>> proposedCosts(
                                new std::unordered_map<unsigned int, CostElements>());

                        unsigned int sawMultipleLayouts;
                        currentLayoutEstimator.estimateCostForQuery(0, sawMultipleLayouts, currentCosts.get());
                        proposedLayoutEstimator.estimateCostForQuery(0, sawMultipleLayouts, proposedCosts.get());

                        assert(currentCosts->size() == proposedCosts->size());
                        std::unordered_set<unsigned int> gopsToRetile;
                        for (auto curIt = currentCosts->begin(); curIt != currentCosts->end(); ++curIt) {
                            auto curCosts = curIt->second;
                            auto newCosts = proposedCosts->at(curIt->first);
                            if (newCosts.numPixels <= 0.6 * curCosts.numPixels)
                                gopsToRetile.insert(curIt->first);
                        }
                        std::copy_if(framesToRetile.begin(), framesToRetile.end(),
                                     std::back_inserter(framesWithDifferentLayout),
                                     [&](int frame) {
                                         return gopsToRetile.count(proposedLayoutEstimator.gopForFrame(frame));
                                     });
                    } else if (node.retileStrategy() == logical::RetileStrategy::RetileAlways) {
                        std::copy_if(framesToRetile.begin(), framesToRetile.end(),
                                     std::back_inserter(framesWithDifferentLayout),
                                     [&](int frame) {
                                         return locationProvider->tileLayoutForFrame(frame) !=
                                                configProvider->tileLayoutForFrame(frame);
                                     });
                    } else {
                        assert(false);
                    }

                    assert(node.entry()->sources().size() == 1);


                    auto logical = plan().lookup(node);
                    auto scan = plan().emplace<physical::ScanFramesFromFileEncodedReader>(logical,
                                                                                          node.entry()->sources()[0]);
                    scan.downcast<physical::ScanFramesFromFileEncodedReader>().setFramesToRead(
                            framesWithDifferentLayout);
                    scan.downcast<physical::ScanFramesFromFileEncodedReader>().setShouldReadEntireGOPs(true);

                    if (framesWithDifferentLayout.size()) {
                        auto gpu = plan().allocator().gpu();
                        auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu);
                        auto crack = plan().emplace<physical::CrackVideo>(
                                logical,
                                decode,
                                std::unordered_set<int>(),
                                configProvider,
                                node.tileLayoutsManager()->entry().name(),
                                gopLength);
                        plan().emplace<physical::Sink>(logical, crack);
                    } else {
                        plan().emplace<physical::Sink>(logical, scan);
                    }
                } else {
                    // Build a query plan for each retiling operation.
                    std::vector<PhysicalOperatorReference> crackOperators;
                    auto logical = plan().lookup(node);
                    auto source = node.entry()->sources()[0];
                    auto gpu = plan().allocator().gpu();
                    std::unique_ptr<std::unordered_map<unsigned int, std::shared_ptr<tiles::TileConfigurationProvider>>> gopToConfigProvider(new std::unordered_map<unsigned int, std::shared_ptr<tiles::TileConfigurationProvider>>());
                    std::vector<int> framesWithDifferentLayout;

                    if (node.retileStrategy() == logical::RetileStrategy::RetileBasedOnRegret) {
                        // Retiling based on regret.
                        static const double pixelCostWeight = 1.608e-06;
                        static const double tileCostWeight = 1.703e-01;
                        assert(node.regretAccumulator());
                        auto workload = std::make_shared<Workload>(node.metadataManager()->metadataIdentifier(),
                                          node.metadataManager()->metadataSpecification());
                        int gopLength = node.entry()->sources()[0].configuration().framerate.fps();
                        WorkloadCostEstimator currentLayoutEstimator(locationProvider, *workload, gopLength,
                                                                     pixelCostWeight, tileCostWeight, 0);
                        std::shared_ptr<std::unordered_map<unsigned int, CostElements>> currentCosts(
                                new std::unordered_map<unsigned int, CostElements>());

                        unsigned int sawMultipleLayouts;
                        currentLayoutEstimator.estimateCostForQuery(0, sawMultipleLayouts, currentCosts.get());
                        node.regretAccumulator()->addRegretForQuery(workload, currentCosts);

//                        for (const auto &layoutId : node.regretAccumulator()->layoutIdentifiers()) {
//                            WorkloadCostEstimator proposedLayoutEstimator(
//                                    node.regretAccumulator()->configurationProviderForIdentifier(layoutId),
//                                    *workload, gopLength, pixelCostWeight, tileCostWeight, 0);
//
//                            std::unique_ptr<std::unordered_map<unsigned int, CostElements>> proposedCosts(
//                                    new std::unordered_map<unsigned int, CostElements>());
//
//                            proposedLayoutEstimator.estimateCostForQuery(0, sawMultipleLayouts, proposedCosts.get());
//
//                            assert(currentCosts->size() == proposedCosts->size());
//
//                            for (auto curIt = currentCosts->begin(); curIt != currentCosts->end(); ++curIt) {
//                                auto curCosts = curIt->second;
//                                auto possibleCosts = proposedCosts->at(curIt->first);
//                                double regret = pixelCostWeight *
//                                                (long long int) (curCosts.numPixels - possibleCosts.numPixels) +
//                                                tileCostWeight * (int) (curCosts.numTiles - possibleCosts.numTiles);
//                                node.regretAccumulator()->addRegretToGOP(curIt->first, regret, layoutId);
//                            }
//                        }

                        // Find the various layouts to retile to.
                        std::unordered_map<unsigned int, std::string> gopToLayoutId;
                        for (auto it = currentCosts->begin(); it != currentCosts->end(); ++it) {
                            auto gopNum = it->first;
                            std::string idForGOP;
                            if (node.regretAccumulator()->shouldRetileGOP(gopNum, idForGOP)) {
                                gopToLayoutId[gopNum] = idForGOP;
                                node.regretAccumulator()->resetRegretForGOP(gopNum);
                            }
                        }

                        for (auto frame : framesToRetile) {
                            auto gop = currentLayoutEstimator.gopForFrame(frame);
                            if (gopToLayoutId.count(gop)) {
                                if (!gopToConfigProvider->count(gop)) {
                                    gopToConfigProvider->emplace(gop,
                                                                 node.regretAccumulator()->configurationProviderForIdentifier(
                                                                         gopToLayoutId.at(gop)));
                                }
                                framesWithDifferentLayout.push_back(frame);
                            }
                        }
                    } else if (node.retileStrategy() == logical::RetileStrategy::RetileAroundMoreObjects) {
                        assert(node.tileAroundMoreObjectsManager());
                        std::unordered_set<int> seenGOPs;
                        for (auto frame : framesToRetile) {
                            int gop = frame / gopLength;
                            if (seenGOPs.count(gop) && gopToConfigProvider->count(gop))
                                framesWithDifferentLayout.push_back(frame);
                            else if (!seenGOPs.count(gop)) {
                                seenGOPs.insert(gop);
                                auto configProvider = node.tileAroundMoreObjectsManager()->configurationProviderForGOPWithQuery(gop, node.metadataManager()->metadataSpecification().objects());
                                if (configProvider) {
                                    gopToConfigProvider->emplace(gop, configProvider);
                                    framesWithDifferentLayout.push_back(frame);
                                }
                            }
                        }
                    }

                    auto scan = plan().emplace<physical::ScanFramesFromFileEncodedReader>(logical, source);
                    scan.downcast<physical::ScanFramesFromFileEncodedReader>().setFramesToRead(framesWithDifferentLayout);
                    scan.downcast<physical::ScanFramesFromFileEncodedReader>().setShouldReadEntireGOPs(true);

                    if (framesWithDifferentLayout.empty()) {
                        plan().emplace<physical::Sink>(logical, scan);
                    } else {
                        auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu);

                        auto tileConfig = std::make_shared<tiles::ConglomerationTileConfigurationProvider>(std::move(gopToConfigProvider), gopLength);
                        auto crack = plan().emplace<physical::CrackVideo>(
                                logical,
                                decode,
                                std::unordered_set<int>(),
                                tileConfig,
                                node.tileLayoutsManager()->entry().name()
                                );
                        plan().emplace<physical::Sink>(logical, crack);
                    }
                }

                return true;
            }
            return false;
        }

        bool visit(const logical::ScannedMultiTiledLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                // Insert a place-holder operator because we won't know what files to decode / how many decoders to initialize
                // until we find out what the next operator is.
                plan().emplace<physical::ScanMultiTilePlaceholderOperator>(plan().lookup(node));
                return true;
            }
            return false;
        }

        bool visit(const logical::ScannedByGOPLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                assert(node.entry().sources().size() == 1);

                auto gpu = plan().allocator().gpu();
                auto logical = plan().lookup(node);
//                auto scan = plan().emplace<physical::ScanFramesFromFileEncodedReader>(logical, node.entry().sources()[0]);
                auto &scan = plan().emplace<physical::ScanSingleFileDecodeReader>(logical, node.entry().sources()[0]);
                plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu);
                return true;
            }
            return false;
        }

        bool visit(const logical::ScannedTiledLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                assert(!node.entry().sources().empty());

                auto logical = plan().lookup(node);
                for (const auto &stream : node.entry().sources())
                    plan().emplace<physical::ScanNonSequentialFramesFromFileEncodedReader>(logical, stream);

                return true;
            }

            return false;
        }

        bool visit(const logical::ScannedLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                if(node.entry().sources().empty())
                    LOG(WARNING) << "Attempt to decode a catalog entry with no underlying streams";

                for(const auto &stream: node.entry().sources()) {
                    //auto stream = node.entry().streams()[0];
                    auto logical = plan().lookup(node);

                    if((stream.codec() == Codec::h264() ||
                        stream.codec() == Codec::hevc()) &&
                       !plan().environment().gpus().empty()) {
                        auto gpu = plan().allocator().gpu();
//                        auto gpu = plan().environment().gpus()[0];

                        // Only decode certain frames.
//                        plan().emplace<physical::ScanSequentialFramesFromFileEncodedReader>(logical, stream);
//                        plan().emplace<physical::ScanNonSequentialFramesFromFileEncodedReader>(logical, stream);
//                        return true;

                        auto &scan = node.willReadEntireEntry() ?
                                plan().emplace<physical::ScanSingleFileDecodeReader>(logical, stream)
                                : plan().emplace<physical::ScanNonSequentialFramesFromFileEncodedReader>(logical, stream);
                        auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu);
//                        auto &scan = plan().emplace<physical::ScanNonSequentialFramesFromFileEncodedReader>(logical, stream);

                        auto children = plan().children(plan().lookup(node));
                        if (children.size() > 1) {
                            auto tees = physical::TeedPhysicalOperatorAdapter::make(decode, children.size());
                            for (auto index = 0u; index < children.size(); index++)
                                plan().add(tees->physical(index));
                            //plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), decode);
                        }
                    } else if(stream.codec() == Codec::h264() ||
                              stream.codec() == Codec::hevc()) {
                        auto &scan = plan().emplace<physical::ScanSingleFileDecodeReader>(logical, stream);
                        auto decode = plan().emplace<physical::CPUDecode>(logical, scan);

                        auto children = plan().children(plan().lookup(node));
                        if(children.size() > 1) {
                            auto tees = physical::TeedPhysicalOperatorAdapter::make(decode, children.size());
                            for (auto index = 0u; index < children.size(); index++)
                                plan().add(tees->physical(index));
                        }
                    } else if(stream.codec() == Codec::boxes()) {
                        auto &scan = plan().emplace<physical::ScanSingleFile<sizeof(Rectangle) * 8192>>(logical, stream);
                        auto decode = plan().emplace<physical::CPUFixedLengthRecordDecode<Rectangle>>(logical, scan);
                    } else
                        throw NotImplementedError("Unsupported codec when assigning decoder");
                }

                return true;
            }
            return false;
        }

        bool visit(const logical::ExternalLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto logical = plan().lookup(node);

                if(node.codec() == Codec::h264() ||
                   node.codec() == Codec::hevc()) {
                    //auto gpu = plan().environment().gpus()[0];
                    auto gpu = plan().allocator().gpu();

                    auto &scan = plan().emplace<physical::ScanSingleFileDecodeReader>(logical, node.source());
                    auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu);

                    auto children = plan().children(plan().lookup(node));
                    if(children.size() > 1) {
                        auto tees = physical::TeedPhysicalOperatorAdapter::make(decode, children.size());
                        for (auto index = 0u; index < children.size(); index++)
                            plan().add(tees->physical(index));
                            //plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), decode);
                    }
                } else if(node.codec() == Codec::boxes()) {
                    plan().emplace<physical::ScanSingleBoxesFile>(logical, node.source());
//                    auto &scan = plan().emplace<physical::ScanSingleFile<sizeof(Rectangle) * 8192>>(logical, node.source());
//                    auto decode = plan().emplace<physical::CPUFixedLengthRecordDecode<Rectangle>>(logical, scan);
                }

                return true;
            }
            return false;
        }
    };

    class ChooseEncoders : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::MetadataEncodedLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                assert(physical_parents.size() == 1);

                plan().emplace<physical::GPUEncodeToCPU>(plan().lookup(node), physical_parents.front(), node.codec());
                return true;
            }
            return false;
        }

        bool visit(const logical::EncodedLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                //TODO clean this up, shouldn't just be randomly picking last parent
                auto physical_parent = physical_parents[0];
                auto logical = plan().lookup(node);

                if(physical_parent.is<physical::GPUAngularSubquery>() && physical_parent.downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>())
                    plan().emplace<physical::CPUIdentity>(logical, physical_parent);
                else if(physical_parent.is<physical::GPUOperator>() && node.codec().nvidiaId().has_value())
                    plan().emplace<physical::GPUEncodeToCPU>(logical, physical_parent, node.codec());
                else if(physical_parent.is<physical::GPUOperator>() && node.codec() == Codec::raw())
                    plan().emplace<physical::GPUEnsureFrameCropped>(plan().lookup(node), physical_parent);
                else if(physical_parent.is<physical::CPUMap>() && physical_parent.downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name())
                    plan().emplace<physical::CPUIdentity>(logical, physical_parent);
                //TODO this is silly -- every physical operator should declare an output type and we should just use that
                else if(physical_parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && physical_parent->parents()[0].is<physical::CPUMap>() && physical_parent->parents()[0].downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name())
                    plan().emplace<physical::CPUIdentity>(logical, physical_parent);
                else if(physical_parent->device() == physical::DeviceType::CPU) {
                    //auto gpu = plan().environment().gpus()[0];
                    auto gpu = plan().allocator().gpu();
                    auto transfer = plan().emplace<physical::CPUtoGPUTransfer>(logical, physical_parent, gpu);
                    plan().emplace<physical::GPUEncodeToCPU>(plan().lookup(node), transfer, node.codec());
                } else
                    return false;
                return true;
            } else {

            }
            return false;
        }
    };

    class ChooseUnion : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        std::optional<PhysicalOperatorReference> FindEncodeParent(const LightFieldReference &node) {
            for(const auto &parent: node->parents()) {
                if(parent.is<logical::EncodedLightField>() &&
                   plan().has_physical_assignment(parent))
                    return {plan().assignments(parent).front()};
            }
            return {};
        }

        std::optional<PhysicalOperatorReference> FindHomomorphicAncestor(const LightFieldReference &node) {
            std::deque<PhysicalOperatorReference> queue;

            for(const auto &parent: node->parents())
                for(const auto &assignment: plan().assignments(parent))
                    queue.push_back(assignment);

            while(!queue.empty()) {
                auto element = queue.front();
                queue.pop_front();

                if(element->logical().is<logical::CompositeLightField>() &&
                   element.is<physical::HomomorphicUniformAngularUnion>())
                    return element;
                else if(element.is<physical::CPUIdentity>())
                    for(const auto &parent: element->parents())
                        queue.push_back(parent);
            }

            return {};
        }

        bool TryGPUBoxOverlayUnion(const logical::CompositeLightField &node) {
            auto leafs0 = plan().unassigned(node.parents()[0]);
            auto leafs1 = plan().unassigned(node.parents()[1]);

            /*if(leafs0.size() == 2 && leafs1.empty()) {
                leafs1.push_back(leafs0[1]);
                leafs0.pop_back();
            }*/

            std::cout << "***here!" << std::endl;

            bool shouldPerformBoxesUnion = true;

            if(leafs0.size() != 1 || leafs1.size() != 1)
                return false;
            else if (!shouldPerformBoxesUnion) {
                //TODO shouldn't arbitrarily require a shallow union
                if (!node.parents()[0].is<logical::ScannedLightField>() ||
                    !node.parents()[1].is<logical::ScannedLightField>())
                    return false;
                    //TODO should pay attention to all streams
                else if (node.parents()[0].downcast<logical::ScannedLightField>().entry().sources()[0].codec() !=
                         Codec::boxes())
                    return false;
                else if (node.parents()[1].downcast<logical::ScannedLightField>().entry().sources()[0].codec() !=
                         Codec::h264() &&
                         node.parents()[1].downcast<logical::ScannedLightField>().entry().sources()[0].codec() !=
                         Codec::hevc())
                    return false;
            } else {
                if (!(node.parents()[0].is<logical::TransformedLightField>() || node.parents()[0].is<logical::ExternalLightField>()) || !node.parents()[1].is<logical::ExternalLightField>())
                    return false;
                else if (node.parents()[1].downcast<logical::ExternalLightField>().sources()[0].codec() != Codec::hevc())
                    return false;
                else if (node.parents()[0].is<logical::ExternalLightField>() && node.parents()[0].downcast<logical::ExternalLightField>().sources()[0].codec() != Codec::boxes())
                    return false;
                // Assume that the transformed light field returns boxes.
            }

                auto unioned = plan().emplace<physical::GPUBoxOverlayUnion>(
                        plan().lookup(node),
                        std::vector<PhysicalOperatorReference>{leafs0[0], leafs1[0]});

                auto children = plan().children(plan().lookup(node));
                if(children.size() > 1) {
                    auto tees = physical::TeedPhysicalOperatorAdapter::make(unioned, children.size());
                    for (auto index = 0u; index < children.size(); index++) {
                        if (unioned->device() == physical::DeviceType::CPU)
                            plan().assign(plan().lookup(node), tees->physical(index));
                        else if (unioned->device() == physical::DeviceType::GPU)
                            plan().add(tees->physical(index));
                            //plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), unioned);
                        else
                            throw InvalidArgumentError("No rule support for device type.", "node");
                    }
                }
                
                return true;

        }

        bool visit(const logical::CompositeLightField &node) override {
            if (plan().has_physical_assignment(node))
                return false;
            else if (node.parents().size() != 2)
                return false;
            else if (node.parents()[0].is<logical::EncodedLightField>() &&
                     node.parents()[1].is<logical::EncodedLightField>()) {
                std::vector<PhysicalOperatorReference> physical_outputs;
                for (auto &parent: node.parents()) {
                    const auto &assignments = plan().assignments(parent);
                    if (assignments.empty())
                        return false;
                    //physical_outputs.push_back(assignments[assignments.size() - 1]);
                    physical_outputs.push_back(assignments.back());
                }

                plan().emplace<physical::HomomorphicUniformAngularUnion>(plan().lookup(node), physical_outputs, 4, 4);
                return true;
            } else if (FindHomomorphicAncestor(node).has_value() &&
                       FindEncodeParent(node).has_value()) {
                auto href = FindHomomorphicAncestor(node).value();
                auto eref = FindEncodeParent(node).value();

                href->parents().emplace_back(eref);
                plan().assign(node, href);
                plan().assign(node, eref);

                plan().emplace<physical::CPUIdentity>(plan().lookup(node), href);
                return true;
            } else if(TryGPUBoxOverlayUnion(node)) {
                return true;
            } else
                return false;
        }
    };

    class ChoosePredicate : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::PredicateLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });
                assert(physical_parents.size() == 1);
                auto parent = physical_parents[0];

                // Add GPU -> CPU.
                auto transfer = plan().emplace<physical::GPUtoCPUTransfer>(plan().lookup(node), parent);

                // Add Predicate.
                plan().emplace<physical::PredicateOperator>(plan().lookup(node), transfer, node.outName());
                return true;
            }
            return false;
        }
    };

    class ChooseMap : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        PhysicalOperatorReference Map(const logical::TransformedLightField &node, PhysicalOperatorReference parent) {
            auto logical = plan().lookup(node);

            if(!node.functor()->has_implementation(physical::DeviceType::GPU)) {
                auto transfer = plan().emplace<physical::GPUtoCPUTransfer>(plan().lookup(node), parent);
                return plan().emplace<physical::CPUMap>(plan().lookup(node), transfer, *node.functor());
            } else
                return plan().emplace<physical::GPUMap>(plan().lookup(node), parent, *node.functor());
        }


        bool visit(const logical::TransformedLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(!physical_parents.empty()) {
                    auto mapped = Map(node, physical_parents[0]);

                    //TODO what if function isn't determistic?!

                    auto children = plan().children(plan().lookup(node));
                    if(children.size() > 1) {
                        auto tees = physical::TeedPhysicalOperatorAdapter::make(mapped, children.size());
                        for(auto index = 0u; index < children.size(); index++) {
                            if(mapped->device() == physical::DeviceType::CPU)
                                plan().assign(plan().lookup(node), tees->physical(index));
                            else if(mapped->device() == physical::DeviceType::GPU)
                                plan().add(tees->physical(index));
                                //plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), mapped);
                            else
                                throw InvalidArgumentError("No rule support for device type.", "node");
                        }
                    }

                    return true;
                }
            }
            return false;
        }
    };

    class ChooseMetadataSelection : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::FrameSubsetLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                assert(physical_parents.size() == 1);

                auto &parent = physical_parents[0];
                auto &grandparent = parent->parents()[0];
                assert(grandparent.is<physical::ScanFramesFromFileEncodedReader>());
                grandparent.downcast<physical::ScanFramesFromFileEncodedReader>().setFramesToRead(node.frameSpecification()->framesToRead());

                plan().emplace<physical::NaiveSelectFrames>(plan().lookup(node), parent, node.frameSpecification()->framesToReadAsASet());
                return true;
            }
            return false;
        }

        bool detectAndMask(const logical::MetadataSubsetLightFieldWithoutSources &node, std::vector<PhysicalOperatorReference> &physical_parents) {
            auto logical = plan().lookup(node);
            auto gpu = plan().allocator().gpu();

            assert(physical_parents.size() == 1);
            assert(physical_parents[0].is<physical::ScanMultiTilePlaceholderOperator>());

            auto &multiTiledLightField = physical_parents[0].downcast<physical::ScanMultiTilePlaceholderOperator>().multiTiledLightField();
            auto metadataManager = node.metadataManager();
            auto &metadataSpecification = metadataManager->metadataSpecification();

            auto tileLayoutsManager = multiTiledLightField.tileLayoutsManager();
            auto tileLocationProvider = std::make_shared<tiles::SingleTileLocationProvider>(tileLayoutsManager);

            bool shouldReadEntireGOPs = true;
            auto scan = plan().emplace<physical::ScanMultiTileOperator>(
                    physical_parents[0]->logical(),
                    metadataManager,
                    tileLocationProvider,
                    shouldReadEntireGOPs);
            bool isDecodingDifferentSizes = !multiTiledLightField.usesOnlyOneTile() && !tileLayoutsManager->hasASingleTile();
            auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu, isDecodingDifferentSizes, tileLayoutsManager->largestWidth(), tileLayoutsManager->largestHeight());

            auto &last = decode;
            // If any frames still have to be detected, add a YOLO map.
            if (metadataManager->anyFramesLackDetections()) {
                assert(node.functor());
                auto map = plan().emplace<physical::GPUMap>(logical, decode, *node.functor());
                last = map;
            }

            // Add bounding boxes around the detected objects.
            auto boxed = plan().emplace<physical::GPUMetadataTransform<video::GPURectangleOverlay>>(logical, last, metadataSpecification, tileLocationProvider);

            // Mask background pixels.
            auto masked = plan().emplace<physical::GPUMetadataTransform<video::GPUSelectPixels>>(logical, boxed, metadataSpecification, tileLocationProvider);

            // Encode modified tiles.
            auto encoded = plan().emplace<physical::GPUEncodeTilesToCPU>(logical, masked, Codec::hevc(), tileLocationProvider);

            // Save modified tiles to disk.
            // Assume that GOP length == fps.
            auto tiledInfo = video::gpac::load_metadata(tileLocationProvider->locationOfTileForFrame(0, 0),
                                                        false,
                                                        {Volume::zero()},
                                                        {GeometryReference::make<EquirectangularGeometry>(EquirectangularGeometry::Samples())});
            auto gopLength = tiledInfo[0].configuration().framerate.fps();
            auto maskedTileLocationProvider = std::make_shared<tiles::MaskedTileLocationProvider>(tileLocationProvider, gopLength);
            auto muxed = plan().emplace<physical::StoreEncodedTiles>(logical, encoded, tileLayoutsManager->entry().path(), Codec::hevc(), maskedTileLocationProvider);

            // Stitch tiles.
            auto stitched = plan().emplace<physical::StitchOperator>(logical, muxed, maskedTileLocationProvider, gopLength, metadataSpecification.firstFrame(), metadataSpecification.lastFrame());

            plan().remove_operator(physical_parents[0]);

            return true;
        }

        bool visit(const logical::MetadataSubsetLightFieldWithoutSources &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                if (node.subsetType() == MetadataSubsetType::MetadataSubsetTypePixelsInFrame)
                    return detectAndMask(node, physical_parents);

                auto logical = plan().lookup(node);
                auto gpu = plan().allocator().gpu();

                assert(physical_parents.size() == 1);
                assert(physical_parents[0].is<physical::ScanMultiTilePlaceholderOperator>());

                // TODO: Find out what frames will be read for selection.
                // Pick a tile configuration for each frame.
                // Create scan/decode operators for each possible tile.
                // Feed the correct frames to each scan. Scans will need to know tile configuration, directory for each frame.
                // Decoders will have to know tile configuration for each tile to decode properly.

                auto &multiTiledLightField = physical_parents[0].downcast<physical::ScanMultiTilePlaceholderOperator>().multiTiledLightField();
                auto metadataManager = node.metadataManager();

                auto tileLayoutsManager = multiTiledLightField.tileLayoutsManager();
                auto tileLocationProvider = std::make_shared<tiles::SingleTileLocationProvider>(tileLayoutsManager);
//                auto tileLocationProvider = std::make_shared<tiles::MultiTileLocationProvider>(tileLayoutsManager, metadataManager, 30);

                // To measure sizes of tiles.
//                auto measure = plan().emplace<physical::MeasureStorageOperator>(physical_parents[0]->logical(), tileLayoutsManager);
//                auto sink = plan().emplace<physical::Sink>(logical, measure);
//                plan().remove_operator(physical_parents[0]);
//                return true;

                auto scan = plan().emplace<physical::ScanMultiTileOperator>(
                        physical_parents[0]->logical(),
                        metadataManager,
                        tileLocationProvider,
                        node.shouldReadEntireGOPs(),
                        node.subsetType() == MetadataSubsetType::MetadataSubsetTypeFrame);
                bool isDecodingDifferentSizes = !multiTiledLightField.usesOnlyOneTile() && !tileLayoutsManager->hasASingleTile();
                auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scan, gpu, isDecodingDifferentSizes, tileLayoutsManager->largestWidth(), tileLayoutsManager->largestHeight());

                // TODO: Should we place the cracking operator in between decode and merge?
                // And then the cracking operator could keep a reference to the frames, but pass them on immediately.
                // Make sure the encoding is happening on different threads.
                // TODO:  Need to get output entry name from multi-tiledlight field.

                if (node.shouldCrack()) {
                    std::shared_ptr<tiles::TileConfigurationProvider> tileConfig = std::make_shared<tiles::GroupingTileConfigurationProvider>(
                            30, // TODO: Don't hardcode tile layout duration.
                            node.metadataManager(),
                            tileLayoutsManager->totalWidth(),
                            tileLayoutsManager->totalHeight());

                    auto crack = plan().emplace<physical::CrackVideo>(logical, decode, std::unordered_set<int>(),
                                                                      tileConfig, tileLayoutsManager->entry().name());
                    auto merge = plan().emplace<physical::MergeTilePixels>(logical, crack, tileLocationProvider);
//                    plan().emplace<physical::SaveFramesToFiles>(logical, merge);
                } else {
//                    auto merge = plan().emplace<physical::MergeTilePixels>(logical, decode, tileLocationProvider);
//                    plan().emplace<physical::SaveFramesToFiles>(logical, merge);
//                    plan().emplace<physical::Sink>(logical, decode);
                }

                // Add a merge operator whose parents are the decodes.
                // Start by assuming that its parents will be in the order of tiles.
//                auto merge = plan().emplace<physical::MergeTilePixels>(logical, crack, tileLocationProvider);
//                plan().emplace<physical::SaveFramesToFiles>(logical, merge);

                plan().remove_operator(physical_parents[0]);

                return true;
            }

            return false;
        }

        bool visit(const logical::MetadataSubsetLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if (physical_parents.empty())
                    return false;

//                assert(physical_parents.size() == 1);
                auto parent = physical_parents.front(); // ScanEncodedFrames

                /* See if we are selecting pixels. */
                if (node.subsetType() & MetadataSubsetTypePixel) {
                    bool isSelectingPixelsAlone = node.subsetType() == MetadataSubsetTypePixel;

                    // There should be a parent for each tile.
                    // Find out what frames are wanted for metadata.
                    // Get rectangle for tile.
                    auto logical = plan().lookup(node);

                    std::vector<PhysicalOperatorReference> lastPerTileOperators;
                    std::unordered_map<int, int> indexToTileNumber;
                    std::unique_ptr<tiles::TileLayout> tileLayoutPtr;

                    // If there is only one parent, then video isn't tiled.
                    // Just set frames to decode on parent's scan and add SelectPixels operator.
                    if (physical_parents.size() == 1 && !isSelectingPixelsAlone) {
                        assert(physical_parents[0]->parents().size() == 1);
                        auto &parent = physical_parents[0];
                        auto &scanParent = parent->parents()[0];
                        auto &scan = scanParent.downcast<physical::ScanFramesFromFileEncodedReader>();
                        scan.setFramesToRead(node.orderedFramesForMetadata());
                        plan().emplace<physical::GPUNaiveSelectPixels>(logical, parent, 0, tiles::NoTilesLayout);

                        return true;
                    } else if (physical_parents.size() == 1 && isSelectingPixelsAlone) {
                        assert(physical_parents[0]->parents().size() == 1);
                        auto &parent = physical_parents[0];
                        auto &scanParent = parent->parents()[0];
                        auto &scan = scanParent.downcast<physical::ScanFramesFromFileEncodedReader>();
                        scan.setFramesToRead(node.orderedFramesForMetadata());
                        std::cout << "***numberOfFrames," << node.source().mp4Reader().numberOfSamples() << std::endl;
//                        std::cout << "Frames: ";
//                        for (auto frame : node.orderedFramesForMetadata())
//                            std::cout << frame << ", ";
//                        std::cout << std::endl;
//                        auto merge = plan().emplace<physical::MergeTilePixels>(logical, physical_parents, tiles::NoTilesLayout, std::unordered_map<int, int>());
//                        plan().emplace<physical::SaveFramesToFiles>(logical, merge);
                        plan().emplace<physical::Sink>(logical, parent);

                        return true;
                    }

                    auto gpu = plan().allocator().gpu();
                    for (auto &parent: physical_parents) {
                        auto &scan = parent.downcast<physical::ScanFramesFromFileEncodedReader>();
                        auto tileNumber = scan.source().index();

                        auto &tileLayout = parent->logical().downcast<logical::ScannedTiledLightField>().entry().tileLayout();
                        if (!tileLayoutPtr)
                            tileLayoutPtr = std::make_unique<tiles::TileLayout>(tiles::TileLayout(tileLayout));

                        std::vector<int> framesForTileAndMetadata = node.framesForTileAndMetadata(tileNumber,
                                                                                                  tileLayout);

                        // Assign these frames to the scan operator.
                        scan.setFramesToRead(framesForTileAndMetadata);
                        // For the case where there are fewer frames than in the metadata table.
                        scan.setGlobalFramesToRead(node.orderedFramesForMetadata());


                        // Decode the frames, perform pixel selection, then encode the frames.
                        // Tile number = parent.to(scan).source().index()
                        auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, parent, gpu);

                        if (isSelectingPixelsAlone) {
                            indexToTileNumber[lastPerTileOperators.size()] = tileNumber;
                            lastPerTileOperators.push_back(decode);
                        } else {
                            // TODO: Put select pixels operator in here.
                            auto selectPixels = plan().emplace<physical::GPUNaiveSelectPixels>(logical, decode,
                                                                                               tileNumber, tileLayout);

                            // TODO: For now, assume encode returns one GOP-worth of data at a time.
                            auto encode = plan().emplace<physical::GPUEncodeToCPU>(logical, selectPixels,
                                                                                   Codec::hevc());
                            encode.downcast<physical::GPUEncodeToCPU>().setDesiredKeyframes(
                                    scan.source().mp4Reader().keyframeNumbers());

                            // Add tile aggregator.
                            auto &tileEntry = parent->logical().downcast<logical::ScannedTiledLightField>().entry();
                            lastPerTileOperators.emplace_back(
                                    plan().emplace<physical::CoalesceSingleTile>(logical, encode, scan.framesToRead(),
                                                                                 node.orderedFramesForMetadata(),
                                                                                 tileEntry, tileNumber));
                        }
                    }

                    // TODO: Add operator to interleave nals from different tiles.
                    if (isSelectingPixelsAlone) {
                        assert(tileLayoutPtr);
                        auto merge = plan().emplace<physical::MergeTilePixels>(
                                logical,
                                lastPerTileOperators,
                                *tileLayoutPtr,
                                indexToTileNumber);
                    } else {
                        plan().emplace<physical::StitchTiles>(
                                logical,
                                lastPerTileOperators,
                                physical_parents.front()->logical().downcast<logical::ScannedTiledLightField>().entry().tileLayout());
                    }

                    return true;
                }

                /* For combo selection */
                if (physical_parents.size() == 2) {
                    bool frontIsNonSequentialScan = parent.is<physical::ScanNonSequentialFramesFromFileEncodedReader>();
                    auto &scanNonSequentialFrames = frontIsNonSequentialScan
                                                    ? physical_parents.front().downcast<physical::ScanNonSequentialFramesFromFileEncodedReader>()
                                                    : physical_parents.back().downcast<physical::ScanNonSequentialFramesFromFileEncodedReader>();
                    auto &scanSequentialFrames = frontIsNonSequentialScan
                                                 ? physical_parents.back().downcast<physical::ScanSequentialFramesFromFileEncodedReader>()
                                                 : physical_parents.front().downcast<physical::ScanSequentialFramesFromFileEncodedReader>();

                    auto sequentialAndNonSequentialFrames = node.sequentialFramesAndNonSequentialFrames();
                    scanSequentialFrames.setFramesToRead(sequentialAndNonSequentialFrames.first);
                    scanNonSequentialFrames.setFramesToRead(sequentialAndNonSequentialFrames.second);

                    auto gpu = plan().allocator().gpu();
                    auto logical = plan().lookup(node);
                    auto decode = plan().emplace<physical::GPUDecodeFromCPU>(logical, scanNonSequentialFrames, gpu);
                    auto encode = plan().emplace<physical::GPUEncodeToCPU>(logical, decode, Codec::hevc());

                    encode.downcast<physical::GPUEncodeToCPU>().setFramesToKeep(scanNonSequentialFrames.framesToRead());
                    encode.downcast<physical::GPUEncodeToCPU>().setDesiredKeyframes(
                            metadata::MetadataManager::idealKeyframesForFrames(scanNonSequentialFrames.framesToRead()));
                    return true;
                }

                /* For homomorphic selection */
                if (parent.is<physical::ScanFramesFromFileEncodedReader>()) {
                    auto &scanFrames = parent.downcast<physical::ScanFramesFromFileEncodedReader>();
                    plan().emplace<physical::HomomorphicSelectFrames>(plan().lookup(node), parent, scanFrames.source());
                    scanFrames.setFramesToRead(node.orderedFramesForMetadata());
                    return true;
                }

                /* For non-homorphic selection (decode & encode everything) */
                plan().emplace<physical::NaiveSelectFrames>(plan().lookup(node), parent, node.framesForMetadata());
                auto &grandParent = parent->parents()[0];
                if (grandParent.is<physical::ScanFramesFromFileEncodedReader>()) {
                    auto &scan = grandParent.downcast<physical::ScanFramesFromFileEncodedReader>();
                    scan.setFramesToRead(node.orderedFramesForMetadata());
                }

                return true;
            } else
                return false;
        }
    };

    class ChooseSelection : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        PhysicalOperatorReference AngularSelection(const logical::SubsetLightField &node,
                                                     PhysicalOperatorReference parent) {
            if(parent->device() == physical::DeviceType::GPU)
                return plan().emplace<physical::GPUAngularSubframe>(plan().lookup(node), parent);
            else
                throw std::runtime_error("Hardcoded support only for GPU angular selection"); //TODO
        }

        PhysicalOperatorReference TemporalSelection(const logical::SubsetLightField &node,
                                                      PhysicalOperatorReference parent) {
            LOG(WARNING) << "Assuming temporal selection parent is encoded video without actually checking";
            if(parent->device() == physical::DeviceType::GPU)
                return plan().emplace<physical::FrameSubset>(plan().lookup(node), parent);
            else
                throw std::runtime_error("Hardcoded support only for GPU temporal selection"); //TODO
        }

        PhysicalOperatorReference IdentitySelection(const logical::SubsetLightField &node,
                                                      PhysicalOperatorReference parent) {
            if(parent->device() == physical::DeviceType::CPU)
                return plan().emplace<physical::CPUIdentity>(plan().lookup(node), parent);
            else if(parent->device() == physical::DeviceType::GPU)
                return plan().emplace<physical::GPUIdentity>(plan().lookup(node), parent);
            else
                throw std::runtime_error("No identity support for FPGA"); //TODO
        }

        bool visit(const logical::SubsetLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                auto selection = physical_parents[0];
                auto dimensions = node.dimensions();

                selection = dimensions.find(Dimension::Theta) != dimensions.end() ||
                            dimensions.find(Dimension::Phi) != dimensions.end()
                    ? AngularSelection(node, selection)
                    : selection;

                selection = dimensions.find(Dimension::Time) != dimensions.end()
                    ? TemporalSelection(node, selection)
                    : selection;

                selection = dimensions.empty()
                    ? IdentitySelection(node, selection)
                    : selection;

                if(dimensions.find(Dimension::X) != dimensions.end() ||
                        dimensions.find(Dimension::Y) != dimensions.end() ||
                        dimensions.find(Dimension::Z) != dimensions.end())
                    throw std::runtime_error("Missing support for spatial selection"); //TODO

                return selection != physical_parents[0];
            }
            return false;
        }
    };

    class ChooseInterpolate : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::InterpolatedLightField &node) override {
            auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                    node.parents().begin(), node.parents().end(),
                    [this](auto &parent) { return plan().assignments(parent); });

            if(physical_parents.empty())
                return false;

            //TODO clean this up, shouldn't just be randomly picking last parent
            auto hardcoded_parent = physical_parents[0].is<physical::GPUDecodeFromCPU>() || physical_parents[0].is<physical::CPUtoGPUTransfer>()
                                    ? physical_parents[0]
                                    : physical_parents[physical_parents.size() - 1];

            if(!plan().has_physical_assignment(node)) {
                if(hardcoded_parent->device() != physical::DeviceType::GPU)
                    throw std::runtime_error("Hardcoded support only for GPU interpolation"); //TODO
                //if(!node.interpolator()->has_implementation(physical::DeviceType::GPU)) {
                plan().emplace<physical::GPUInterpolate>(plan().lookup(node), hardcoded_parent, node.interpolator());
                return true;
            }
            return false;
        }
    };

    class ChooseDiscretize : public OptimizerRule {
        void teeIfNecessary(const LightField& node, PhysicalOperatorReference physical) {
            auto children = plan().children(plan().lookup(node));
            if(children.size() > 1) {
                auto tees = physical::TeedPhysicalOperatorAdapter::make(physical, children.size());
                for (auto index = 0u; index < children.size(); index++) {
                    if (physical->device() == physical::DeviceType::CPU)
                        plan().assign(plan().lookup(node), tees->physical(index));
                    else if (physical->device() == physical::DeviceType::GPU)
                        plan().add(tees->physical(index));
                        //plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), physical);
                    else
                        throw InvalidArgumentError("No rule support for device type.", "node");
                }
            }
        }


    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::DiscreteLightField &node) override {
            auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                    node.parents().begin(), node.parents().end(),
                    [this](auto &parent) { return plan().assignments(parent); });

            if (physical_parents.empty())
                return false;

            //TODO clean this up, shouldn't just be randomly picking last parent
            auto hardcoded_parent = physical_parents[0].is<physical::GPUDecodeFromCPU>() ||
                                    physical_parents[0].is<physical::CPUtoGPUTransfer>()
                                    ? physical_parents[0]
                                    : physical_parents[physical_parents.size() - 1];
            auto is_discrete = (hardcoded_parent.is<physical::GPUDecodeFromCPU>() &&
                                hardcoded_parent->logical().is<logical::ScannedLightField>()) ||
                               hardcoded_parent.is<physical::GPUDownsampleResolution>();

            if (!plan().has_physical_assignment(node) && is_discrete &&
                hardcoded_parent.is<physical::GPUDownsampleResolution>()) {
                auto &discrete = node.downcast<logical::DiscreteLightField>();
                hardcoded_parent.downcast<physical::GPUDownsampleResolution>().geometries().push_back(static_cast<IntervalGeometry&>(*discrete.geometry()));
                // Was CPUIdentity, bug or intentional?
                auto identity = plan().emplace<physical::GPUIdentity>(plan().lookup(node), hardcoded_parent);
                teeIfNecessary(node, identity);
                return true;
            } else if(!plan().has_physical_assignment(node) && is_discrete) {
                auto downsampled = hardcoded_parent->logical().try_downcast<logical::DiscretizedLightField>();
                auto scanned = downsampled.has_value() ? hardcoded_parent->logical()->parents()[0].downcast<logical::ScannedLightField>() : hardcoded_parent->logical().downcast<logical::ScannedLightField>();
                auto &parent_geometry = scanned.entry().sources().front().geometry();
                auto &discrete_geometry = node.geometry();

                if(scanned.entry().sources().size() != 1)
                    return false;
                else if(!discrete_geometry.is<IntervalGeometry>())
                    return false;
                else if(!parent_geometry.is<EquirectangularGeometry>())
                    return false;
                else if((discrete_geometry.downcast<IntervalGeometry>().dimension() == Dimension::Theta &&
                         scanned.entry().sources()[0].configuration().width % discrete_geometry.downcast<IntervalGeometry>().size().value_or(1) == 0) ||
                        (discrete_geometry.downcast<IntervalGeometry>().dimension() == Dimension::Phi &&
                         scanned.entry().sources()[0].configuration().height % discrete_geometry.downcast<IntervalGeometry>().size().value_or(1) == 0))
                {
                    if(hardcoded_parent->device() == physical::DeviceType::GPU)
                    {
                        auto downsample = plan().emplace<physical::GPUDownsampleResolution>(plan().lookup(node), hardcoded_parent, discrete_geometry.downcast<IntervalGeometry>());
                        teeIfNecessary(node, downsample);
                        return true;
                    }
                }
                //TODO handle case where interval is equal to resolution (by applying identity)
            } else if (!plan().has_physical_assignment(node) && hardcoded_parent.is<physical::GPUDecodeFromCPU>() && hardcoded_parent->logical().is<logical::ExternalLightField>()) {
                auto scanned = hardcoded_parent->logical().downcast<logical::ExternalLightField>();
                auto &parent_geometry = scanned.source().geometry();
                auto &discrete_geometry = node.geometry();
                assert(discrete_geometry.is<IntervalGeometry>());
                assert(parent_geometry.is<EquirectangularGeometry>());
                assert(discrete_geometry.downcast<IntervalGeometry>().dimension() == Dimension::Time);
                assert(hardcoded_parent->device() == physical::DeviceType::GPU);
                plan().emplace<physical::GPUDownsampleResolution>(plan().lookup(node), hardcoded_parent, discrete_geometry.downcast<IntervalGeometry>());
                return true;
            }
            return false;
        }
    };

    class ChooseLinearScale : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::DiscreteLightField &node) override {
            auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                    node.parents().begin(), node.parents().end(),
                    [this](auto &parent) { return plan().assignments(parent); });

            if(physical_parents.empty())
                return false;

            if (node.geometry().is<IntervalGeometry>() && node.geometry().downcast<IntervalGeometry>().dimension() == Dimension::Time)
                return false;

            //TODO clean this up, shouldn't just be randomly picking last parent
            auto hardcoded_parent = physical_parents[0].is<physical::GPUDecodeFromCPU>() || physical_parents[0].is<physical::CPUtoGPUTransfer>()
                                    ? physical_parents[0]
                                    : physical_parents[physical_parents.size() - 1];
            auto hardcoded_grandparent = hardcoded_parent->parents()[0];
            auto hardcoded_greatgrandparent = hardcoded_grandparent->parents()[0];
            auto is_linear_interpolated =
                    hardcoded_parent.is<physical::GPUInterpolate>() &&
                    hardcoded_parent->logical().downcast<logical::InterpolatedLightField>().interpolator()->name() == "linear";

            if(!plan().has_physical_assignment(node) && is_linear_interpolated) {
                auto scanned = hardcoded_grandparent->logical().is<logical::ScannedLightField>() ? hardcoded_grandparent->logical().downcast<logical::ScannedLightField>() : hardcoded_greatgrandparent->logical().downcast<logical::ScannedLightField>();
                auto &scanned_geometry = scanned.entry().sources().front().geometry();
                auto &discrete_geometry = node.geometry();

                if(scanned.entry().sources().size() != 1)
                    return false;
                else if(!discrete_geometry.is<IntervalGeometry>())
                    return false;
                else if(!scanned_geometry.is<EquirectangularGeometry>())
                    return false;
                else if(discrete_geometry.downcast<IntervalGeometry>().dimension() == Dimension::Theta ||
                        discrete_geometry.downcast<IntervalGeometry>().dimension() == Dimension::Phi)
                {
                    if(hardcoded_parent->device() == physical::DeviceType::GPU)
                    {
                        plan().emplace<physical::GPUDownsampleResolution>(plan().lookup(node), hardcoded_parent, discrete_geometry.downcast<IntervalGeometry>());
                        return true;
                    }
                }
                //TODO handle case where interval is equal to resolution (by applying identity)
            }
            return false;
        }
    };

    class ChoosePartition : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const logical::PartitionedLightField &node) override {
            auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                    node.parents().begin(), node.parents().end(),
                    [this](auto &parent) { return plan().assignments(parent); });

            if(physical_parents.empty())
                return false;

            //TODO clean this up, shouldn't just be randomly picking last parent
            //OMG this is even more horrible now
            size_t index;
            if(physical_parents.size() == 1)
                index = 0;
            else if(physical_parents.size() == 2) {
                // Pick the most parentiest of parents, wtf
                if(!physical_parents[0]->parents().empty() && physical_parents[0]->parents()[0] == physical_parents[1])
                    index = 0;
                else
                    index = 1;
            } else
                index = physical_parents.size() - 1;
            //auto hardcoded_parent = physical_parents[physical_parents.size() - 1];
            auto hardcoded_parent = physical_parents[index];

            if(!plan().has_physical_assignment(node)) {
                if(hardcoded_parent->device() == physical::DeviceType::CPU)
                    plan().emplace<physical::CPUIdentity>(plan().lookup(node), hardcoded_parent);
                else if(hardcoded_parent.is<physical::GPUOperator>())
                    plan().emplace<physical::GPUIdentity>(plan().lookup(node), hardcoded_parent);
                return true;
            }
            return false;
        }
    };

    class ChooseSubquery : public OptimizerRule {
        void teeIfNecessary(const LightField& node, const PhysicalOperatorReference &physical) {
            auto children = plan().children(plan().lookup(node));
            if(children.size() > 1) {
                auto tees = physical::TeedPhysicalOperatorAdapter::make(physical, children.size());
                for (auto index = 0u; index < children.size(); index++) {
                    plan().assign(plan().lookup(node), tees->physical(index));
                    /*if (physical->device() == physical::DeviceType::CPU)
                        plan().assign(plan().lookup(node), tees->physical(index));
                    else if (physical->device() == physical::DeviceType::GPU)
                        plan().emplace<physical::GPUOperatorAdapter>(tees->physical(index), physical);
                    else
                        throw InvalidArgumentError("No rule support for device type.", "node");*/
                }
            }
        }

    public:
        explicit ChooseSubquery(const OptimizerReference &optimizer)
            : optimizer_(optimizer)
        { }

        bool visit(const logical::SubqueriedLightField &node) override {
            auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                    node.parents().begin(), node.parents().end(),
                    [this](auto &parent) { return plan().assignments(parent); });

            if(physical_parents.empty())
                return false;

            //TODO clean this up, shouldn't just be randomly picking last parent
            auto hardcoded_parent = physical_parents[0];

            if(!plan().has_physical_assignment(node)) {
                auto subquery = plan().emplace<physical::GPUAngularSubquery>(
                        plan().lookup(node), hardcoded_parent, optimizer_);
                teeIfNecessary(node, subquery);
                return true;
            }
            return false;
        }

    private:
        const OptimizerReference optimizer_;
    };

    class ChooseStore : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        PhysicalOperatorReference Encode(const logical::StoredLightField &node, PhysicalOperatorReference parent, bool replace=false) {
            auto logical = plan().lookup(node);

            // Can we leverage the ChooseEncode rule to automatically do this stuff, which is an exact duplicate?

            if(parent.is<physical::GPUAngularSubquery>() && parent.downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            //} else if(parent.is<physical::GPUOperatorAdapter>() && parent->parents()[0].is<physical::GPUAngularSubquery>() && parent->parents()[0].downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
            //    return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUEncodeToCPU>()) {
                return plan().emplace<physical::GPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUOperator>()) {
//                auto ensureCropped = plan().emplace<physical::GPUEnsureFrameCropped>(logical, parent);
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
            } else if(parent.is<physical::CPUMap>() && parent.downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
                //TODO this is silly -- every physical operator should declare an output type and we should just use that
            } else if(parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && parent->parents()[0].is<physical::CPUMap>() && parent->parents()[0].downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && parent->parents()[0].is<physical::GPUAngularSubquery>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent->device() != physical::DeviceType::GPU) {
                //auto gpu = plan().environment().gpus()[0];
                auto gpu = plan().allocator().gpu();
                auto transfer = plan().emplace<physical::CPUtoGPUTransfer>(logical, parent, gpu);
                return plan().emplace<physical::GPUEncodeToCPU>(logical, transfer, Codec::hevc());
            } else if(!parent.is<physical::GPUOperator>()) {
                //auto gpuop = plan().emplace<physical::GPUOperatorAdapter>(parent);
                //return plan().emplace<physical::GPUEncodeToCPU>(logical, gpuop, Codec::hevc());
                if (replace) {
                    auto encode = PhysicalOperatorReference::make<physical::GPUEncodeToCPU>(logical, parent->parents().front(), Codec::hevc());
                    plan().replace_assignments(parent, encode);
                    return encode;
                } else
                    return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
            } else
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
        }

        bool visit(const logical::CrackedLightField &node) override {
            if (!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                assert(physical_parents.size() == 1);

                // Add an encode operator.
                // Tell it the tile configuration for each GOP.
                auto logical = plan().lookup(node);
                std::unordered_set<int> keyframes;
                unsigned int width;
                unsigned int height;
                unsigned int fps;
                if (physical_parents[0]->parents()[0].is<physical::ScanSingleFileDecodeReader>()) {
                    auto &source = physical_parents[0]->parents()[0].downcast<physical::ScanSingleFileDecodeReader>().source();
//                    keyframes.insert(source.keyframes().begin(), source.keyframes().end());
                    width = source.configuration().width;
                    height = source.configuration().height;
                    fps = source.configuration().framerate.fps();
                } else if (physical_parents[0]->parents()[0].is<physical::ScanFramesFromFileEncodedReader>()) {
                    auto &source = physical_parents[0]->parents()[0].downcast<physical::ScanFramesFromFileEncodedReader>().source();
                    width = source.configuration().width;
                    height = source.configuration().height;
                    fps = source.configuration().framerate.fps();
                } else
                    assert(false);

                std::shared_ptr<tiles::TileConfigurationProvider> tileConfig;
                unsigned int layoutDuration = node.layoutDuration();
                if (node.metadataManager()) {
//                    auto tileLayoutDuration = 60; // TODO: Make layout duration argument to CrackedLightField.
                    assert(node.layoutDuration());
                    layoutDuration = node.layoutDuration();
                    assert(node.crackingStrategy() != CrackingStrategy::None);
                    if (node.crackingStrategy() == CrackingStrategy::SmallTiles) {
                        tileConfig = std::make_shared<tiles::GroupingTileConfigurationProvider>(
                                node.layoutDuration(),
                                node.metadataManager(),
                                width,
                                height);
                    } else if (node.crackingStrategy() == CrackingStrategy::GroupingExtent) {
                        tileConfig = std::make_shared<tiles::GroupingExtentsTileConfigurationProvider>(
                                node.layoutDuration(),
                                node.metadataManager(),
                                width,
                                height);
                    } else {
                        // Unrecognized cracking strategy.
                        assert(false);
                    }
                } else if (node.crackingStrategy() == CrackingStrategy::Uniform) {
                    layoutDuration = fps;
                    tileConfig = std::make_shared<tiles::UniformTileconfigurationProvider>(node.uniformDimensionsCols(), node.uniformDimensionsRows(), width, height);
                } else if (node.crackingStrategy() == CrackingStrategy::OneTile) {
                    tileConfig = std::make_shared<tiles::SingleTileConfigurationProvider>(width, height);
                } else if (node.crackingStrategy() == CrackingStrategy::ROI) {
                    tileConfig = std::make_shared<tiles::ROITileConfigurationProvider>(width, height, node.roi());
                } else {
                    assert(false);
                }

                auto crack = plan().emplace<physical::CrackVideo>(
                                 logical,
                                 physical_parents[0],
                                 keyframes,
                                 tileConfig,
                                 "",
                                 layoutDuration);
                // TODO: Add encode & store for each tile.

//                auto encode = plan().emplace<physical::GPUEncodeToCPU>(logical, physical_parents[0], Codec::hevc());
                // TODO: Make this store tiles-by-first-and-last-frame & include tile configuration in metadata.
                // Can't simply store by GOP because once we store multiple versions, GOP won't make sense because there
                // could be overlapping GOPs.
//                plan().emplace<physical::Store>(logical, encode);
                return true;
            }
            return false;
        }

        bool visit(const logical::StoredLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

                /* For combo metadata frame selection */
//                auto encodeParent = physical_parents.front();
//                auto scanParentOfEncode = encodeParent->parents().front()->parents().front();
//                assert(scanParentOfEncode.is<physical::ScanNonSequentialFramesFromFileEncodedReader>());
//
//                bool shouldIncludeDecodeEncodeInPlan = !scanParentOfEncode.downcast<physical::ScanNonSequentialFramesFromFileEncodedReader>().framesToRead().empty();
//
//                // Find scan of sequential frames.
//                auto logicalScan = node.parents().front()->parents().front();
//                auto physicalAssignmentsForScanLogical = plan().assignments(logicalScan);
//                assert(physicalAssignmentsForScanLogical.size() == 2);
//                // TODO: This is super hacky.
//                // Find scan physical operator for sequential frames, which is not the parent of encode.
//                auto scanSequentialFrames = physicalAssignmentsForScanLogical.begin()->is<physical::ScanSequentialFramesFromFileEncodedReader>()
//                                                ? *(physicalAssignmentsForScanLogical.begin()) :  *(++physicalAssignmentsForScanLogical.begin());
//                assert(scanSequentialFrames.is<physical::ScanSequentialFramesFromFileEncodedReader>());
//
//                // Get frames that Store is looking for.
//                assert(encodeParent->logical().is<logical::MetadataSubsetLightField>());
//
//                plan().emplace<physical::StoreOutOfOrderData>(plan().lookup(node),
//                        shouldIncludeDecodeEncodeInPlan ? std::vector<PhysicalOperatorReference>({encodeParent, scanSequentialFrames}) : std::vector<PhysicalOperatorReference>({scanSequentialFrames}),
//                        encodeParent->logical().downcast<logical::MetadataSubsetLightField>().orderedFramesForMetadata());
//                return true;
//
//                // For homomorphic frame selection:
//                plan().emplace<physical::Store>(plan().lookup(node), physical_parents.front());
//                return true;
//
//                // For naive frame selection:
//                bool lastFrameWasMetadataSelection = physical_parents.front().is<physical::NaiveSelectFrames>();
//                auto encode = Encode(node, physical_parents.front(), lastFrameWasMetadataSelection);
//                if (lastFrameWasMetadataSelection) {
//                    encode.downcast<physical::GPUEncodeToCPU>().setFramesToKeep(physical_parents.front().downcast<physical::NaiveSelectFrames>().framesToKeep());
//                }
//                plan().emplace<physical::Store>(plan().lookup(node), encode);
//                return true;

                auto encode = Encode(node, physical_parents[0]);
                plan().emplace<physical::Store>(plan().lookup(node), encode);
                return true;
            }
            return false;
        }
    };

    class ChooseSink : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        PhysicalOperatorReference Encode(const logical::SunkLightField &node, PhysicalOperatorReference parent) {
            auto logical = plan().lookup(node);

            // Can we leverage the ChooseEncode rule to automatically do this stuff, which is an exact duplicate?
            //TODO Copied from ChooseStore, which is lame

            if(parent.is<physical::GPUAngularSubquery>() && parent.downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
                //} else if(parent.is<physical::GPUOperatorAdapter>() && parent->parents()[0].is<physical::GPUAngularSubquery>() && parent->parents()[0].downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
                //    return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUEncodeToCPU>()) {
                return plan().emplace<physical::GPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUOperator>()) {
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
                //TODO this is silly -- every physical operator should declare an output type and we should just use that
            } else if(parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && parent->parents()[0].is<physical::GPUAngularSubquery>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent->device() != physical::DeviceType::GPU) {
                //auto gpu = plan().environment().gpus()[0];
                auto gpu = plan().allocator().gpu();
                auto transfer = plan().emplace<physical::CPUtoGPUTransfer>(logical, parent, gpu);
                return plan().emplace<physical::GPUEncodeToCPU>(logical, transfer, Codec::hevc());
            } else if(!parent.is<physical::GPUOperator>()) {
                //auto gpuop = plan().emplace<physical::GPUOperatorAdapter>(parent);
                //return plan().emplace<physical::GPUEncodeToCPU>(logical, gpuop, Codec::hevc());
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
            } else
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
        }

        bool visit(const logical::SunkLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

//                auto sink = Encode(node, physical_parents.front());
                plan().emplace<physical::Sink>(plan().lookup(node), physical_parents);
                return true;
            }
            return false;
        }
    };

    class ChooseSave : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        //TODO Duplicate of ChooseStore::Encode, this is lame
        PhysicalOperatorReference Encode(const logical::SavedLightField &node, PhysicalOperatorReference parent, bool replace=false) {
            auto logical = plan().lookup(node);

            // Can we leverage the ChooseEncode rule to automatically do this stuff, which is an exact duplicate?

            if(parent.is<physical::GPUAngularSubquery>() && parent.downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
                //} else if(parent.is<physical::GPUOperatorAdapter>() && parent->parents()[0].is<physical::GPUAngularSubquery>() && parent->parents()[0].downcast<physical::GPUAngularSubquery>().subqueryType().is<logical::EncodedLightField>()) {
                //    return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUEncodeToCPU>()) {
                return plan().emplace<physical::GPUIdentity>(logical, parent);
            } else if(parent.is<physical::GPUOperator>()) {
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
            } else if(parent.is<physical::CPUMap>() && parent.downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
                //TODO this is silly -- every physical operator should declare an output type and we should just use that
            } else if (parent.is<physical::CPUMap>() && node.filename().extension() == ".boxes") {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && parent->parents()[0].is<physical::CPUMap>() && parent->parents()[0].downcast<physical::CPUMap>().transform()(physical::DeviceType::CPU).codec().name() == node.codec().name()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            } else if(parent.is<physical::TeedPhysicalOperatorAdapter::TeedPhysicalOperator>() && parent->parents()[0].is<physical::GPUAngularSubquery>()) {
                return plan().emplace<physical::CPUIdentity>(logical, parent);

            } else if(parent->device() != physical::DeviceType::GPU) {
                //auto gpu = plan().environment().gpus()[0];
                auto gpu = plan().allocator().gpu();
                auto transfer = plan().emplace<physical::CPUtoGPUTransfer>(logical, parent, gpu);
                return plan().emplace<physical::GPUEncodeToCPU>(logical, transfer, Codec::hevc());
            } else if(!parent.is<physical::GPUOperator>()) {
                //auto gpuop = plan().emplace<physical::GPUOperatorAdapter>(parent);
                //return plan().emplace<physical::GPUEncodeToCPU>(logical, gpuop, Codec::hevc());
                if (replace) {
                    auto encode = PhysicalOperatorReference::make<physical::GPUEncodeToCPU>(logical, parent->parents().front(), Codec::hevc());
                    plan().replace_assignments(parent, encode);
                    return encode;
                } else
                    return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
            } else
                return plan().emplace<physical::GPUEncodeToCPU>(logical, parent, Codec::hevc());
        }

        bool visit(const logical::SavedLightField &node) override {
            if(!plan().has_physical_assignment(node)) {
                auto physical_parents = functional::flatmap<std::vector<PhysicalOperatorReference>>(
                        node.parents().begin(), node.parents().end(),
                        [this](auto &parent) { return plan().unassigned(parent); });

                if(physical_parents.empty())
                    return false;

//                // For homomorphic frame selection:
                plan().emplace<physical::SaveToFile>(plan().lookup(node), physical_parents.front());
                return true;

                bool lastFrameWasMetadataSelection = physical_parents.front().is<physical::NaiveSelectFrames>();

                auto encode = Encode(node, physical_parents.front(), lastFrameWasMetadataSelection);

                if (lastFrameWasMetadataSelection) {
                    encode.downcast<physical::GPUEncodeToCPU>().setFramesToKeep(physical_parents.front().downcast<physical::NaiveSelectFrames>().framesToKeep());
                }

               if (physical_parents.front().is<physical::CPUMap>() && node.filename().extension() == ".boxes") {
                   plan().emplace<physical::SaveBoxes>(std::shared_ptr<metadata::MetadataManager>(),plan().lookup(node), encode);
               } else {
                   plan().emplace<physical::SaveToFile>(plan().lookup(node), encode);
               }
               return true;
            }
            return false;
        }
    };

    class RemoveIdentities : public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool visit(const LightField &node) override {
            auto logical = plan().lookup(node);
            auto assignments = plan().assignments(logical);

            for(auto &assignment: assignments) {
                if(assignment.is<physical::GPUIdentity>() ||
                   assignment.is<physical::CPUIdentity>()) {
                    auto &parents = assignment->parents();

                    CHECK_EQ(parents.size(), 1);

                    auto parent = parents.at(0);

                    parents.clear();
                    plan().replace_assignments(assignment, parent);

                    return true;
                }
            }

            return false;
        }
    };

    class RemoveDegenerateDecodeEncode: public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool IsParentADecodeOperator(const LightField &node) {
            return node.parents().size() == 1 &&
                   plan().assignments(node.parents().front()).size() == 1 &&
                   plan().assignments(node.parents().front()).front().is<physical::GPUDecodeFromCPU>();
        }

        //TODO Uh, implement this
        bool IsCompatibleCodecConfiguration() {
            return true;
        }

        PhysicalOperatorReference CreateIdentity(const LightFieldReference& logical, PhysicalOperatorReference &parent) {
            if(parent->device() == physical::DeviceType::CPU)
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            else if(parent->device() == physical::DeviceType::GPU)
                return plan().emplace<physical::GPUIdentity>(logical, parent);
            else
                throw InvalidArgumentError("Unsupported device type for identity creation", "parent");
        }

        bool visit(const logical::EncodedLightField &node) override {
            auto logical = plan().lookup(node);

            if(IsParentADecodeOperator(*logical) &&
               IsCompatibleCodecConfiguration()) {
                auto &encode = logical;
                auto &decode = logical->parents().front();
                auto out = plan().children(logical);

                auto decode_physical = plan().assignments(decode).front();
                auto encode_physical = plan().assignments(logical).front();
                auto source_physical = functional::single(decode_physical->parents()); //plan().assignments(source).front();

                auto decode_identity = CreateIdentity(decode, source_physical);
                auto encode_identity = CreateIdentity(encode, decode_identity);

                // Remove encode
                plan().replace_assignments(encode_physical, encode_identity);

                // Remove decode
                plan().replace_assignments(decode_physical, decode_identity);

                return true;
            }

            return false;
        }
    };

    class ConvertLoadSaveToCopy: public OptimizerRule {
    public:
        using OptimizerRule::OptimizerRule;

        bool IsPhysicalChildASaveOperator(const LightFieldReference &node) {
            const auto children = plan().children(node);
            return children.size() == 1 &&
                   plan().assignments(children.front()).size() == 1 &&
                   plan().assignments(children.front()).front().is<physical::ScanSingleFileDecodeReader>();
        }

        PhysicalOperatorReference CreateIdentity(const LightFieldReference& logical, PhysicalOperatorReference &parent) {
            if(parent->device() == physical::DeviceType::CPU)
                return plan().emplace<physical::CPUIdentity>(logical, parent);
            else if(parent->device() == physical::DeviceType::GPU)
                return plan().emplace<physical::GPUIdentity>(logical, parent);
            else
                throw InvalidArgumentError("Unsupported device type for identity creation", "parent");
        }

        bool ApplyRule(const LightField &node) {
            auto logical = plan().lookup(node);

            if(IsPhysicalChildASaveOperator(logical)) {
                auto &scan = logical;

                auto scan_physical = plan().assignments(scan).front();
                auto save_physical =  plan().children(scan_physical).front();
                auto in_physical = scan_physical->parents();
                auto out_physical = plan().children(save_physical);

                auto &save = save_physical->logical().downcast<logical::SavedLightField>();

                auto copy_physical = plan().emplace<physical::CopyFile>(
                        scan, std::vector<std::filesystem::path>{save.filename()}, in_physical);
                auto save_identity = CreateIdentity(save, copy_physical);

                plan().replace_assignments(save_physical, save_identity);
                plan().replace_assignments(scan_physical, copy_physical);

                return true;
            }

            return false;
        }

        bool visit(const logical::ExternalLightField &node) override {
            return ApplyRule(node);
        }

        bool visit(const logical::ScannedLightField &node) override {
            return ApplyRule(node);
        }
    };
}

#endif //LIGHTDB_RULES_H
