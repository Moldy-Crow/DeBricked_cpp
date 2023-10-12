#pragma once

#include "layer.hpp"

class MaxPoolingLayer : public Layer {
  public:
    MaxPoolingLayer(const nlohmann::json& obj, size_t inputW, size_t inputH, size_t inputDepth);

    LayerType type() const override { return LayerType::MAX_POOLING; }
    std::array<size_t, 3> outputSize() const override;
    const Vector& activations() const override;
    const Vector& delta() const override;
    void trainForward(const Vector& inputs) override;
    Vector evalForward(const Vector& inputs) const override;
    void updateDelta(const Vector& layerInputs, const Layer& nextLayer, size_t epoch) override;
    nlohmann::json getConfig() const override;
    void writeToStream(std::ostream&) const override {}
    const Matrix& W() const override;

  private:
    Vector m_Z;
    Vector m_delta;
    size_t m_regionW;
    size_t m_regionH;
    size_t m_inputW;
    size_t m_inputH;
    size_t m_inputDepth;
    Vector m_mask;

    void backpropFromDenseLayer(const Layer& nextLayer);
    void backpropFromConvLayer(const Layer& nextLayer);
    void padDelta(const Vector& delta);
};
