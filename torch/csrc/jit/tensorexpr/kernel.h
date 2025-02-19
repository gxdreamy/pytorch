#pragma once

#include <c10/util/variant.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>
#include <torch/csrc/jit/runtime/interpreter.h>
#include <torch/csrc/jit/tensorexpr/analysis.h>
#include <torch/csrc/jit/tensorexpr/codegen.h>
#include <torch/csrc/jit/tensorexpr/lowerings.h>
#include <torch/csrc/jit/tensorexpr/tensor.h>

namespace torch {
namespace jit {
namespace tensorexpr {

// Returns true if the TE fuser supports this conv2d.
bool conv2dIsSupportedJit(const Node* node);
// Returns true if the TE fuser supports this matmul.
bool matmulIsSupported(const Node* node);
template <typename T>
inline std::vector<int64_t> bufferSizes(const T& t) {
  std::vector<int64_t> sizes;
  for (size_t i = 0; i < t->ndim(); i++) {
    sizes.push_back(*intValue(t->dim(i)));
  }
  return sizes;
}

// Get the dimensions of a value.
std::vector<ExprHandle> valueShape(const ArgValue& v);

// If v is a tensor, broadcast it to match the shape of axes, or return
// directly if v is a constant.
ExprHandle tensorOrConstant(
    const ArgValue& v,
    const std::vector<ExprHandle>& axes);

int64_t normalizeAndCheckIndex(int64_t idx, int64_t list_size);

ExprHandle broadcast(BufHandle b, const std::vector<ExprHandle>& axes);

ExprHandle constant(const ArgValue& v);

std::vector<ExprHandle> computeIndicesToBroadcast(
    const std::vector<ExprHandle>& outputAxes,
    const std::vector<ExprHandle>& inputSizes);

inline std::string getArgValueName(const ArgValue& a) {
  if (c10::get_if<tensorexpr::BufHandle>(&a)) {
    return "BufHandle";
  } else if (c10::get_if<tensorexpr::VarHandle>(&a)) {
    return "VarHandle";
  } else if (c10::get_if<double>(&a)) {
    return "double";
  } else if (c10::get_if<int64_t>(&a)) {
    return "int64_t";
  } else if (c10::get_if<bool>(&a)) {
    return "bool";
  } else if (c10::get_if<BufList>(&a)) {
    return "BufList";
  } else if (c10::get_if<DoubleList>(&a)) {
    return "DoubleList";
  } else if (c10::get_if<IntList>(&a)) {
    return "IntList";
  } else if (c10::get_if<ArgNone>(&a)) {
    return "None";
  } else {
    throw std::runtime_error("ArgValue type not handled in string conversion");
  }
}

template <class T>
std::vector<T> convertVecArgValue(const std::vector<ArgValue>& v) {
  std::vector<T> res;
  for (auto& x : v) {
    auto val = c10::get_if<T>(&x);
    if (val) {
      res.push_back(*val);
    } else {
      throw std::runtime_error(
          "vector type not homogeneous - found " + getArgValueName(x) +
          ", expected " + getArgValueName(v[0]));
    }
  }
  return res;
}

class TORCH_API TensorExprKernel {
  struct ConstantDescr {
    BufPtr buf;
    // Only one of ptr and node is used at a time
    // 1) ptr for the constant tensors
    // 2) node for the constant custom class ojects
    void* ptr = nullptr;
    Node* node = nullptr;
  };

 public:
  // Constructor Params:
  //  * subgraph
  //      - the graph that needs to be compiled.
  //  * kernel_func_name
  //      - the name that should be used for the generated kernel.
  //  * custom_lowerings
  //      - map that represents custom lowering definitions for a set of ops.
  //  * symbolic_shape_inputs
  //      - a list of symbolic graph inputs that represent the symbolic dims of
  //        the input tensors.
  //  * pre_alloc
  //      - a flag to control pre-allocation of buffers.
  explicit TensorExprKernel(
      const std::shared_ptr<Graph>& subgraph,
      const std::string& kernel_func_name,
      std::unordered_map<c10::Symbol, NNCLoweringFunction> custom_lowerings =
          {},
      std::vector<int64_t> symbolic_shape_inputs = {},
      bool pre_alloc = false);

  explicit TensorExprKernel(
      const std::shared_ptr<Graph>& subgraph,
      std::unordered_map<c10::Symbol, NNCLoweringFunction> custom_lowerings =
          {},
      std::vector<int64_t> symbolic_shape_inputs = {},
      bool pre_alloc = false)
      : TensorExprKernel(
            subgraph,
            SubgraphUtils::generateNameForGraph(subgraph),
            custom_lowerings,
            symbolic_shape_inputs,
            pre_alloc) {}

  void run(Stack& stack);
  void runFast(
      const std::vector<void*>& inputs,
      const std::vector<void*>& outputs);
  // Expected format of stack:
  //  ... <outputs> <inputs>
  // i.e., output IValues must be below the input IValues in the stack.
  void runWithAllocatedOutputs(Stack& stack);

  void fallback(Stack& stack) {
    InterpreterState(code_).run(stack);
  }
  void recompile();

  StmtPtr getCodeGenStmt();

  std::string getCodeText(const std::string& attr = "") {
    return codegen_->getCodeText(attr);
  }

  const std::shared_ptr<Graph> graph() {
    return graph_;
  }

  const std::vector<ConstantDescr>& getConstantDescriptors() const {
    return constants_;
  }

  const std::vector<CodeGen::BufferArg>& getBufferArgs() const {
    return bufferArgs_;
  }

  const std::string& getKernelName() const {
    return codegen_->kernel_func_name();
  }

 private:
  enum BackendType {
    kUninitialized,
    kSimpleIREval,
    kLLVMCodeGen,
    kCudaCodeGen,
    kBlockCodeGen,
  };

  void compile();
  void genInputDebugNames();
  void runKernel(Stack& stack);

  std::vector<DimArg> dimsFromSizes(const std::vector<ExprHandle>& sizes);
  std::vector<ExprHandle> sizesForValue(const torch::jit::Value* v);

  // These functions broadcast shape and also store a `hasBroadcast_` variable.
  std::vector<ExprHandle> broadcastShapesMut(
      const std::vector<ExprHandle>& a,
      const std::vector<ExprHandle>& b);
  std::vector<ExprHandle> broadcastShapesMut(
      std::vector<std::vector<ExprHandle>> shapes);

  ArgValue toArg(const torch::jit::Value* v) const;
  ExprHandle constant(const torch::jit::Value* v);

  ExprHandle tensorOrConstant(
      const torch::jit::Value* v,
      const std::vector<ExprHandle>& axes);

  Tensor computeValue(const torch::jit::Value* v);

  void bindConstant(const torch::jit::Value* v);

  StmtPtr transformLoops(BackendType backendType, StmtPtr st);

  std::string getCodeGenName(BackendType backendType);

  void updateOutputSizesAndStrides(const at::ArrayRef<IValue>& inputs);
  std::vector<CodeGen::CallArg> prepareRunArgs(
      const at::ArrayRef<IValue>& inputs,
      std::vector<at::Tensor>& outputs);
  BackendType inferBackendTypeFromDevice(at::Device device);

  Tensor bindInput(const torch::jit::Value* input);
  BlockPtr bindAllInputs();

  Tensor convertOutputToCorrectStrides(torch::jit::Value* v);

  NNCLoweringFunction getCustomLoweringFor(c10::Symbol op) const;
  std::unordered_map<c10::Symbol, NNCLoweringFunction> getCustomLowerings()
      const {
    return custom_lowerings_;
  }

  // Allocate memory for intermediate buffers at compile time.
  // Specifically, we pre-allocate memory for intermediate buffers with static
  // size and manage these buffers in the way we manage JIT constant tensors:
  // push the buf args into the stack so NNC IR can access them at runtime.
  std::vector<BufPtr> preAllocIntermediateBufs(
      const std::vector<BufPtr>& interm_bufs);

  struct UnpackedTensorOptions {
    c10::optional<c10::ScalarType> dtype;
    c10::optional<c10::Layout> layout;
    c10::optional<c10::Device> device;
    c10::optional<bool> pinned_memory;

    UnpackedTensorOptions(const c10::TensorOptions& opts)
        : dtype(optTypeMetaToScalarType(opts.dtype_opt())),
          layout(opts.layout_opt()),
          device(opts.device_opt()),
          pinned_memory(opts.pinned_memory_opt()) {}
  };

  ExprHandle getVarForShape(const c10::ShapeSymbol& ss);
  std::vector<ExprHandle> computeInputTensorDims(
      const torch::jit::Value* input);
  std::vector<ExprHandle> sizesFromSymbolicShape(
      const c10::SymbolicShape& shape);

  int64_t nInputs_ = 0;
  int64_t nOutputs_ = 0;
  std::vector<CodeGen::BufferArg> bufferArgs_;
  std::vector<std::vector<int64_t>> tensorOutputSizes_;
  std::vector<std::vector<int64_t>> tensorOutputStrides_;
  std::vector<UnpackedTensorOptions> tensorOutputTensorOptions_;
  std::unordered_set<BufPtr> bufOutputs_;
  std::unordered_map<const torch::jit::Value*, BufPtr> bufs_;
  std::unordered_map<const torch::jit::Value*, VarHandle> scalars_;
  std::unordered_map<const torch::jit::Value*, std::string> input_name_map_;
  std::unique_ptr<CodeGen> codegen_;
  at::Device device_ = at::kCPU;
  std::shared_ptr<Graph> graph_;
  Code code_;
  bool allow_fallback_{false};
  bool use_fallback_{false};
  bool hasRandom_{false};
  bool hasBroadcast_{false};
  std::unordered_map<const torch::jit::Value*, std::vector<ExprHandle>>
      known_sizes_;

  std::vector<std::vector<ExprHandle>> tensorOutputSymbolicSizes_;
  // A map from ShapeSymbol.value() to the corresponding Var.
  std::unordered_map<int64_t, VarHandle> shapeSymbolToVar_;
  std::unordered_map<ExprPtr, size_t> shapeSymbolInputPos_;
  // List of values corresponding to the ShapeSymbols that are inputs to
  // kernel being compiled. The order of these values correspond to the order
  // of the symbolic inputs at the end of the list of inputs to the kernel.
  std::vector<int64_t> symbolic_shape_inputs_;
  bool has_symbolic_shapes_{false};

  std::vector<at::Tensor> unpacked_constant_tensors_;
  std::vector<ConstantDescr> constants_;

  std::unordered_map<c10::Symbol, NNCLoweringFunction> custom_lowerings_;
  StmtPtr stmt_ = nullptr;
  bool pre_alloc_{false};
  std::string kernel_func_name_;
};

TORCH_API int& getTECudaPointwiseLoopLevels();
TORCH_API int& getTECudaPointwiseBlockCount();
TORCH_API int& getTECudaPointwiseBlockSize();
TORCH_API bool& getTEGenerateBlockCode();
TORCH_API bool& getTEMustUseLLVMOnCPU();
TORCH_API bool fallbackAllowed();
TORCH_API bool setFallbackAllowed(bool value);
TORCH_API bool& getCatWoConditionals();
TORCH_API bool& getOptConditionals();

TORCH_API c10::optional<at::Device> pickDeviceType(
    const at::ArrayRef<torch::jit::Value*>& inputs);

} // namespace tensorexpr
} // namespace jit
} // namespace torch
