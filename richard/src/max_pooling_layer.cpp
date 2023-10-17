#include <iostream> // TODO
#include "max_pooling_layer.hpp"
#include "convolutional_layer.hpp"
#include "exception.hpp"

MaxPoolingLayer::MaxPoolingLayer(const nlohmann::json& obj, size_t inputW, size_t inputH,
  size_t inputDepth)
  : m_Z(inputW, inputH, inputDepth)
  , m_delta(inputW, inputH, inputDepth)
  , m_inputW(inputW)
  , m_inputH(inputH)
  , m_inputDepth(inputDepth)
  , m_mask(inputW, inputH, inputDepth) {

  std::array<size_t, 2> regionSize = getOrThrow(obj, "regionSize").get<std::array<size_t, 2>>();
  m_regionW = regionSize[0];
  m_regionH = regionSize[1];

  TRUE_OR_THROW(inputW % m_regionW == 0,
    "Region width " << m_regionW << " does not divide input width " << inputW);
  TRUE_OR_THROW(inputH % m_regionH == 0,
    "Region height " << m_regionH << " does not divide input height " << inputH);
}

const Matrix& MaxPoolingLayer::W() const {
  assert(false);
  static Matrix m(1, 1);
  return m;
}

std::array<size_t, 3> MaxPoolingLayer::outputSize() const {
  return {
    static_cast<size_t>(m_inputW / m_regionW),
    static_cast<size_t>(m_inputH / m_regionH),
    m_inputDepth
  };
}

const DataArray& MaxPoolingLayer::activations() const {
  return m_Z.storage();
}

const DataArray& MaxPoolingLayer::delta() const {
  return m_delta.storage();
}

void MaxPoolingLayer::trainForward(const DataArray& inputs) {/*
  TRUE_OR_THROW(inputs.dimensions() == 3, "Max pooling layer expects 3D input");
  const Array3 image = dynamic_cast<const Array3&>(inputs);

  size_t outputW = m_inputW / m_regionW;
  size_t outputH = m_inputH / m_regionH;

  #pragma omp parallel for
  for (size_t z = 0; z < m_inputDepth; ++z) {
    for (size_t y = 0; y < outputH; ++y) {
      for (size_t x = 0; x < outputW; ++x) {
        double largest = std::numeric_limits<double>::min();
        size_t largestInputX = 0;
        size_t largestInputY = 0;
        for (size_t j = 0; j < m_regionH; ++j) {
          for (size_t i = 0; i < m_regionW; ++i) {
            size_t imgX = x * m_regionW + i;
            size_t imgY = y * m_regionH + j;
            double input = image.at(imgX, imgY, z);
            if (input > largest) {
              largest = input;
              largestInputX = imgX;
              largestInputY = imgY;
            }
            m_mask[imgX, imgY, z] = 0.0;
          }
        }
        m_mask.set(largestInputX, largestInputY, z, 1.0);
        m_Z.set(x, y, z, largest);
      }
    }
  }*/
}

DataArray MaxPoolingLayer::evalForward(const DataArray& inputs) const {/*
  TRUE_OR_THROW(inputs.dimensions() == 3, "Max pooling layer expects 3D input");
  const Array3 image = dynamic_cast<const Array3&>(inputs);

  size_t outputW = m_inputW / m_regionW;
  size_t outputH = m_inputH / m_regionH;
  Array3Ptr Z = std::make_unique<Array3>(outputW, outputH, m_inputDepth);

  #pragma omp parallel for
  for (size_t z = 0; z < m_inputDepth; ++z) {
    for (size_t y = 0; y < outputH; ++y) {
      for (size_t x = 0; x < outputW; ++x) {
        double largest = std::numeric_limits<double>::min();
        for (size_t j = 0; j < m_regionH; ++j) {
          for (size_t i = 0; i < m_regionW; ++i) {
            size_t inputX = x * m_regionW + i;
            size_t inputY = y * m_regionH + j;
            double input = image.at(inputX, inputY, z);
            if (input > largest) {
              largest = input;
            }
          }
        }
        Z->set(x, y, z, largest);
      }
    }
  }

  return Z;*/
}

// Pad the delta to the input size using the mask for ease of consumption by the previous layer
void MaxPoolingLayer::padDelta(const Array3& delta, const Array3& mask, Array3& paddedDelta) const {/*
  size_t outputW = m_inputW / m_regionW;
  size_t outputH = m_inputH / m_regionH;

  #pragma omp parallel for
  for (size_t z = 0; z < m_inputDepth; ++z) {
    for (size_t y = 0; y < outputH; ++y) {
      for (size_t x = 0; x < outputW; ++x) {
        for (size_t j = 0; j < m_regionH; ++j) {
          for (size_t i = 0; i < m_regionW; ++i) {
            size_t imgX = x * m_regionW + i;
            size_t imgY = y * m_regionH + j;
            if (mask[inputOffset + inputY * m_inputW + inputX] != 0.0) {
              paddedDelta[inputOffset + inputY * m_inputW + inputX] = delta[y * outputW + x];
            }
            else {
              paddedDelta[inputOffset + inputY * m_inputW + inputX] = 0.0;
            }
          }
        }
      }
    }
  }*/
}

void MaxPoolingLayer::backpropFromDenseLayer(const Layer& nextLayer, Vector& delta) {
  ConstVectorPtr pNextDelta = Vector::createShallow(nextLayer.delta());
  delta = nextLayer.W().transposeMultiply(*pNextDelta);
}

// TODO
void MaxPoolingLayer::backpropFromConvLayer(const std::vector<ConvolutionalLayer::Filter>& filters,
  const Vector& convDelta, Array3& delta) {
/*
  size_t convLayerDepth = convParams.size();
  size_t kW = convParams[0].W.cols();
  size_t kH = convParams[0].W.rows();
  //double kSz_rp = 1.0 / (kW * kH);

  size_t outputW = m_inputW / m_regionW;
  size_t outputH = m_inputH / m_regionH;

  size_t fmW = outputW - kW + 1;
  size_t fmH = outputH - kH + 1;
  size_t fmSize = fmW * fmH;

  for (size_t fm = 0; fm < convLayerDepth; ++fm) {
    const Matrix& kernel = convParams[fm].W;
    size_t fmOffset = fm * fmSize;

    for (size_t fmY = 0; fmY < fmH; ++fmY) {
      for (size_t fmX = 0; fmX < fmW; ++fmX) {
        for (size_t j = 0; j < kH; ++j) {
          for (size_t i = 0; i < kW; ++i) {
            size_t x = fmX + i;
            size_t y = fmY + j;
            delta[y * outputW + x] +=
              kernel.at(i, j) * convDelta[fmOffset + fmY * fmW + fmX];
          }
        }
      }
    }
  }*/
}

// TODO
void MaxPoolingLayer::updateDelta(const DataArray&, const Layer& nextLayer, size_t) {/*
  size_t outputW = m_inputW / m_regionW;
  size_t outputH = m_inputH / m_regionH;

  // TODO: Make member variable
  Vector delta(outputW * outputH);

  switch (nextLayer.type()) {
    case LayerType::OUTPUT:
    case LayerType::DENSE: {
      backpropFromDenseLayer(nextLayer, delta);
      break;
    }
    case LayerType::CONVOLUTIONAL: {
      const auto& convLayer = dynamic_cast<const ConvolutionalLayer&>(nextLayer);
      backpropFromConvLayer(convLayer.params(), convLayer.delta(), delta);
      break;
    }
    default: {
      EXCEPTION("Expected layer of type DENSE or CONVOLUTIONAL, got " << nextLayer.type());
    }
  }

  padDelta(delta, m_mask, m_delta);

  //std::cout << "Max pooling delta: \n";
  //std::cout << m_delta;
  */
}

nlohmann::json MaxPoolingLayer::getConfig() const {
  nlohmann::json config;
  config["type"] = "maxPooling";
  config["regionSize"] = std::array<size_t, 2>({ m_regionW, m_regionH });
  return config;
}

const Array3& MaxPoolingLayer::mask() const {
  return m_mask;
}
