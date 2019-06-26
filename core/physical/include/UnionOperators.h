#ifndef LIGHTDB_UNIONOPERATORS_H
#define LIGHTDB_UNIONOPERATORS_H

#include "PhysicalOperators.h"
#include "EncodeOperators.h"
#include "MetadataLightField.h"
#include "Rectangles.h"

#include <iostream>

namespace lightdb::physical {

class GPUTileUnion : public PhysicalOperator, public GPUOperator {
public:
    explicit GPUTileUnion(const LightFieldReference &logical,
                          std::vector<PhysicalOperatorReference> &parents,
                          const unsigned int rows, const unsigned int columns)
            : PhysicalOperator(logical, parents, DeviceType::GPU, runtime::make<Runtime>(*this)),
              GPUOperator(parents.front()),
              rows_(rows),
              columns_(columns)
    { }

    GPUTileUnion(const GPUTileUnion &) = delete;

    unsigned int rows() const { return rows_; }
    unsigned int columns() const { return columns_; }

private:
    class Runtime: public runtime::GPURuntime<GPUTileUnion> {
    public:
        explicit Runtime(GPUTileUnion &physical)
            : runtime::GPURuntime<GPUTileUnion>(physical),
              configuration_(create_configuration(physical.rows(), physical.columns())),
              geometry_(get_geometry()),
              frames_(functional::make_union_iterator<GPUFrameReference>(
                      functional::transform<
                              functional::flatmap_iterator<GPUFrameReference,
                                      Runtime::downcast_iterator<
                                      GPUDecodedFrameData>>>(
                              iterators().begin(), iterators().end(),
                                      [](runtime::RuntimeIterator &it) {
                                          return functional::make_flatmap_iterator<GPUFrameReference>(
                                                  it.downcast<GPUDecodedFrameData>()); })))
        { }

        std::optional<physical::MaterializedLightFieldReference> read() override {
            if(!any_parent_eos()) {
                auto available_frames = std::max(frames_.available(), 1ul);
                GPUDecodedFrameData output{configuration_, geometry_};

                for(auto i = 0u; i < available_frames; i++) {
                    auto frames = frames_++;
                    auto unioned = GPUFrameReference::make<CudaFrame>(
                            frames.front()->height() * physical().rows(),
                            frames.front()->width() * physical().columns(),
                            frames.front()->type());
                    auto &cuda = unioned.downcast<CudaFrame>();

                    for(auto column = 0u; column < physical().columns(); column++)
                        for(auto row = 0u; row < physical().rows(); row++)
                            cuda.copy(lock(), *frames[row * physical().columns() + column]->cuda(),
                                      configuration_.height / physical().rows(),
                                      configuration_.width / physical().columns());

                    output.frames().emplace_back(GPUFrameReference{unioned});
                }

                return {output};
            } else
                return {};
        }

    private:
        Configuration create_configuration(const unsigned int rows, const unsigned int columns) {
            CHECK(!physical().parents().empty());

            auto configuration = (*iterators().front()).expect_downcast<FrameData>().configuration();

            return Configuration{configuration.width * columns, configuration.height * rows, 0, 0,
                                 configuration.bitrate, configuration.framerate, configuration.offset};
        }

        GeometryReference get_geometry() {
            CHECK(!physical().parents().empty());

            return (*iterators().front()).expect_downcast<FrameData>().geometry();
        }

        const Configuration configuration_;
        const GeometryReference geometry_;
        functional::union_iterator<
                GPUFrameReference,
                std::vector<
                        functional::flatmap_iterator<GPUFrameReference,
                                Runtime::downcast_iterator<GPUDecodedFrameData>>>> frames_;
    };

    const unsigned int rows_, columns_;
};


template<typename Transform, typename Data>
class GPUOverlayUnion : public PhysicalOperator, public GPUOperator {
public:
    explicit GPUOverlayUnion(const LightFieldReference &logical,
                             std::vector<PhysicalOperatorReference> &parents)
            : PhysicalOperator(logical, parents, DeviceType::GPU, runtime::make<Runtime>(*this)),
              GPUOperator(parents.back())
    { }

    GPUOverlayUnion(const GPUOverlayUnion &) = delete;

private:
    class Runtime: public runtime::GPURuntime<GPUOverlayUnion> {
    public:
        explicit Runtime(GPUOverlayUnion &physical)
            : runtime::GPURuntime<GPUOverlayUnion>(physical),
              transform_(this->context()),
              groups_(this->iterators().front()) {
            CHECK_EQ(physical.parents().size(), 2);
        }

        std::optional<physical::MaterializedLightFieldReference> read() override {
            // FIXME: iterators().back() shouldn't be hardcoded maybe?
            // FIXME: If the boxes iterator is done, but the video one isn't, we should continue
            // and just output the remaining video frames.
            if(this->iterators().back() != this->iterators().back().eos() && groups_.hasMoreValues()) {
                auto video = (this->iterators().back()++).template downcast<GPUDecodedFrameData>();
                GPUDecodedFrameData output{video.configuration(), video.geometry()};

                for(auto &frame: video.frames()) {
                    auto values = groups_++;
                    if (values.size()) {
                        auto unioned = transform_.nv12().draw(this->lock(), frame->cuda(), values);
                        output.frames().emplace_back(unioned);
                    } else
                        output.frames().emplace_back(frame);
                }

                return {output};
            } else
                return {};
        }

    private:
        class GroupById {
        public:
            explicit GroupById(runtime::RuntimeIterator& iterator)
                    : index_(0u),
                      current_id_(0u),
                      buffer_(MaterializedLightFieldReference::make<CPUDecodedFrameData>(
                                  Configuration{}, GeometryReference::make<EquirectangularGeometry>(0, 0))),
                      iterator_(iterator)
            { }

            const std::vector<Data> operator++(int) {
                std::cout << "Finding boxes for frame " << current_id_ << std::endl;

                std::optional<Data> value;
                std::vector<Data> values;

                while((value = peek()).has_value() && value.value().id == current_id_)
                    values.emplace_back(next());

                // This doesn't work when there aren't boxes for every frame.
                current_id_++;
//                if((value = peek()).has_value())
//                    current_id_ = value.value().id;

                return values;
            }

            bool hasMoreValues() {
                return peek().has_value();
            }

        private:
            Data next() {
                std::cout << "Returning value at index " << index_ << " for frame " << current_id_ << std::endl;
                auto value = peek().value();
                index_++;
                return value;
            }

            std::optional<Data> peek() {
                if (buffer_.is<CPUDecodedFrameData>()) {
                    if (index_ >= frames().size()) {
                        buffer_ = iterator_++;

                        while (buffer_.is<MetadataLightField>() && !buffer_.downcast<MetadataLightField>().allRectangles().size() && iterator_ != iterator_.eos()) {
                            buffer_ = iterator_++;
                        }

                        index_ = 0u;
                    }
                } else if (buffer_.is<MetadataLightField>()) {
                    if (index_ >= buffer_.downcast<MetadataLightField>().allRectangles().size()) {
                        if (iterator_ == iterator_.eos()) {
                            return std::nullopt;
                        } else {
                            buffer_ = iterator_++;
                            while (buffer_.is<MetadataLightField>() && !buffer_.downcast<MetadataLightField>().allRectangles().size() && iterator_ != iterator_.eos())
                                buffer_ = iterator_++;
                        }
                        index_ = 0u;
                    }
                } else {
                    assert(0);
                }

                if (buffer_.is<MetadataLightField>()) {
                    MetadataLightField data = buffer_.downcast<MetadataLightField>();
                    Metadata metadata = data.metadata();
                    std::vector<Rectangle> rectangles = data.allRectangles();
                    if (rectangles.empty())
                        return std::nullopt;
                    else
                        return rectangles[index_];
                } else if (buffer_.is<CPUDecodedFrameData>()) {
                    if (!frames().empty())
                        return *reinterpret_cast<const Data *>(frames().at(index_)->data().data());
                    else
                        return std::nullopt;
                } else {
                    assert(0);
                }
            }

            // How to expose buffer as CPUEncodedFrameData?
            inline std::vector<LocalFrameReference>& frames() { return data().frames(); }
            inline CPUDecodedFrameData& data() { return buffer_.downcast<CPUDecodedFrameData>(); }

            size_t index_;
            unsigned int current_id_;
            MaterializedLightFieldReference buffer_;
            runtime::RuntimeIterator &iterator_;
        };

        Transform transform_;
        GroupById groups_;
    };
};

using GPUBoxOverlayUnion = GPUOverlayUnion<video::GPURectangleOverlay, Rectangle>;

} // lightdb::physical

#endif //LIGHTDB_UNIONOPERATORS_H
