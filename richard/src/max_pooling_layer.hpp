#pragma once

#include "layer.hpp"
#include "convolutional_layer.hpp"

class MaxPoolingLayer : public Layer {
  public:
    MaxPoolingLayer(const nlohmann::json& obj, size_t inputW, size_t inputH, size_t inputDepth);

    LayerType type() const override { return LayerType::MAX_POOLING; }
    std::array<size_t, 3> outputSize() const override;
    const DataArray& activations() const override;
    const DataArray& delta() const override;
    void trainForward(const DataArray& inputs) override;
    DataArray evalForward(const DataArray& inputs) const override;
    void updateDelta(const DataArray& inputs, const Layer& nextLayer, size_t epoch) override;
    nlohmann::json getConfig() const override;
    void writeToStream(std::ostream&) const override {}
    const Matrix& W() const override;

    // Exposed for testing
    void padDelta(const Array3& delta, const Array3& mask, Array3& paddedDelta) const;
    const Array3& mask() const;
    void backpropFromConvLayer(const std::vector<ConvolutionalLayer::Filter>& filters,
      const Vector& convDelta, Array3& delta);
    void setWeights(const Matrix&) override { assert(false); }
    void setBiases(const Vector&) override { assert(false); }

  private:
    Array3 m_Z;
    Array3 m_delta;
    size_t m_regionW;
    size_t m_regionH;
    size_t m_inputW;
    size_t m_inputH;
    size_t m_inputDepth;
    Array3 m_mask;

    void backpropFromDenseLayer(const Layer& nextLayer, Vector& delta);
};
