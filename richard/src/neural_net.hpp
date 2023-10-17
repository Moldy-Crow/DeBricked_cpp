#pragma once

#include <vector>
#include <fstream>
#include <array>
#include <nlohmann/json.hpp>
#include "math.hpp"

class LabelledDataSet;

class NeuralNet {
  public:
    using CostFn = std::function<double(const Vector&, const Vector&)>;

    virtual CostFn costFn() const = 0;
    virtual std::array<size_t, 2> inputSize() const = 0;
    virtual void writeToStream(std::ostream& s) const = 0;
    virtual void train(LabelledDataSet& data) = 0;
    virtual VectorPtr evaluate(const Array3& inputs) const = 0;

    // Called from another thread
    virtual void abort() = 0;

    static const nlohmann::json& defaultConfig();

    virtual ~NeuralNet() {}

    // Exposed for testing
    //
    virtual void setWeights(const std::vector<Matrix>& weights) = 0;
    virtual void setBiases(const std::vector<Vector>& biases) = 0;
};

std::unique_ptr<NeuralNet> createNeuralNet(const nlohmann::json& config);
std::unique_ptr<NeuralNet> createNeuralNet(std::istream& fin);
