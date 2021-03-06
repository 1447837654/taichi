// Program, which is a context for a taichi program execution

#include "program.h"

#include <taichi/common/task.h>
#include <taichi/platform/metal/metal_api.h>
#include <taichi/platform/opengl/opengl_api.h>

#include "taichi/codegen/codegen_cuda.h"
#include "taichi/codegen/codegen_metal.h"
#include "taichi/codegen/codegen_opengl.h"
#include "taichi/codegen/codegen_cpu.h"
#include "taichi/struct/struct.h"
#include "taichi/struct/struct_metal.h"
#include "taichi/struct/struct_opengl.h"
#include "taichi/system/unified_allocator.h"
#include "taichi/ir/snode.h"

#include "taichi/backends/cuda/cuda_utils.h"

TLANG_NAMESPACE_BEGIN

void assert_failed_host(const char *msg) {
  TI_ERROR("Assertion failure: {}", msg);
}

void *taichi_allocate_aligned(Program *prog,
                              std::size_t size,
                              std::size_t alignment) {
  return prog->memory_pool->allocate(size, alignment);
}

Program *current_program = nullptr;
std::atomic<int> Program::num_instances;

Program::Program(Arch desired_arch) {
  auto arch = desired_arch;
  if (arch == Arch::cuda) {
    runtime = Runtime::create(arch);
    if (!runtime) {
      TI_WARN("Taichi is not compiled with CUDA.");
      arch = host_arch();
    } else if (!runtime->detected()) {
      TI_WARN("No CUDA device detected.");
      arch = host_arch();
    } else {
      // CUDA runtime created successfully
    }
  }
  if (arch == Arch::metal) {
    if (!metal::is_metal_api_available()) {
      TI_WARN("No Metal API detected.");
      arch = host_arch();
    }
  }
  if (arch == Arch::opengl) {
    if (!opengl::is_opengl_api_available()) {
      TI_WARN("No OpenGL API detected.");
      arch = host_arch();
    }
  }

  if (arch != desired_arch) {
    TI_WARN("Falling back to {}", arch_name(arch));
  }

  memory_pool = std::make_unique<MemoryPool>(this);
  TI_ASSERT_INFO(num_instances == 0, "Only one instance at a time");
  total_compilation_time = 0;
  num_instances += 1;
  SNode::counter = 0;
  // llvm_context_device is initialized before kernel compilation
  TI_ASSERT(current_program == nullptr);
  current_program = this;
  config = default_compile_config;
  config.arch = arch;

  llvm_context_host = std::make_unique<TaichiLLVMContext>(host_arch());
  profiler = make_profiler(arch);

  preallocated_device_buffer = nullptr;

  if (config.enable_profiler && runtime) {
    runtime->set_profiler(profiler.get());
  }

  result_buffer = nullptr;
  current_kernel = nullptr;
  sync = true;
  llvm_runtime = nullptr;
  finalized = false;
  snode_root = std::make_unique<SNode>(0, SNodeType::root);

  TI_TRACE("Program arch={}", arch_name(arch));
}

FunctionType Program::compile(Kernel &kernel) {
  auto start_t = Time::get_time();
  TI_AUTO_PROF;
  FunctionType ret = nullptr;
  if (arch_is_cpu(kernel.arch) || kernel.arch == Arch::cuda) {
    auto codegen = KernelCodeGen::create(kernel.arch, &kernel);
    ret = codegen->compile();
  } else if (kernel.arch == Arch::metal) {
    metal::MetalCodeGen codegen(kernel.name, &metal_struct_compiled_.value());
    ret = codegen.compile(*this, kernel, metal_runtime_.get());
  } else if (kernel.arch == Arch::opengl) {
    opengl::OpenglCodeGen codegen(kernel.name,
                                  &opengl_struct_compiled_.value());
    ret = codegen.compile(*this, kernel);
  } else {
    TI_NOT_IMPLEMENTED;
  }
  TI_ASSERT(ret);
  total_compilation_time += Time::get_time() - start_t;
  return ret;
}

// For CPU and CUDA archs only
void Program::initialize_runtime_system(StructCompiler *scomp) {
  // auto tlctx = llvm_context_host.get();
  TaichiLLVMContext *tlctx;

  std::size_t prealloc_size = 0;

  if (config.arch == Arch::cuda && !config.use_unified_memory) {
#if defined(TI_WITH_CUDA)
    check_cuda_error(cudaMalloc(&result_buffer, sizeof(uint64)));
    auto total_mem = runtime->get_total_memory();
    if (config.device_memory_fraction == 0) {
      TI_ASSERT(config.device_memory_GB > 0);
      prealloc_size = std::size_t(config.device_memory_GB * (1UL << 30));
    } else
      prealloc_size = std::size_t(config.device_memory_fraction * total_mem);

    TI_TRACE("Allocating device memory {:.2f} GB",
             1.0 * prealloc_size / (1UL << 30));

    check_cuda_error(cudaMalloc(&preallocated_device_buffer, prealloc_size));
    check_cuda_error(cudaMemset(preallocated_device_buffer, 0, prealloc_size));
    tlctx = llvm_context_device.get();
#else
    TI_NOT_IMPLEMENTED
#endif
  } else {
    result_buffer = taichi_allocate_aligned(this, 8, 8);
    tlctx = llvm_context_host.get();
  }
  auto runtime = tlctx->runtime_jit_module;

  // By the time this creator is called, "this" is already destroyed.
  // Therefore it is necessary to capture members by values.
  auto snodes = scomp->snodes;
  int root_id = snode_root->id;

  TI_TRACE("Allocating data structure of size {} B", scomp->root_size);

  runtime
      ->call<void *, void *, std::size_t, std::size_t, void *, void *, void *, void *>(
          "runtime_initialize", result_buffer, this,
          (std::size_t)scomp->root_size, prealloc_size,
          preallocated_device_buffer, (void *)&taichi_allocate_aligned,
          (void *)std::printf, (void *)std::vsnprintf);

  TI_TRACE("LLVMRuntime initialized");
  llvm_runtime = runtime->fetch_result<void *>();
  TI_TRACE("LLVMRuntime pointer fetched");

  if (arch_use_host_memory(config.arch) || config.use_unified_memory) {
    runtime->call<void *>("runtime_get_mem_req_queue", llvm_runtime);
    auto mem_req_queue = runtime->fetch_result<void *>();
    memory_pool->set_queue((MemRequestQueue *)mem_req_queue);
  }

  runtime->call<void *, int, int>("runtime_initialize2", llvm_runtime, root_id,
                                  (int)snodes.size());

  for (int i = 0; i < (int)snodes.size(); i++) {
    if (snodes[i]->type == SNodeType::pointer ||
        snodes[i]->type == SNodeType::dynamic) {
      std::size_t node_size;
      if (snodes[i]->type == SNodeType::pointer)
        node_size = tlctx->get_type_size(
            scomp->snode_attr[snodes[i]].llvm_element_type);
      else {
        // dynamic. Allocators are for the chunks
        node_size = sizeof(void *) +
                    tlctx->get_type_size(
                        scomp->snode_attr[snodes[i]].llvm_element_type) *
                        snodes[i]->chunk_size;
      }
      TI_TRACE("Initializing allocator for snode {} (node size {})",
               snodes[i]->id, node_size);
      auto rt = llvm_runtime;
      runtime->call<void *, int, std::size_t>(
          "runtime_NodeAllocator_initialize", rt, i, node_size);
      TI_TRACE("Allocating ambient element for snode {} (node size {})",
               snodes[i]->id, node_size);
      runtime->call<void *, int>("runtime_allocate_ambient", rt, i);
    }
  }

  if (arch_use_host_memory(config.arch)) {
    runtime->call<void *, void *, void *>("LLVMRuntime_initialize_thread_pool",
                                          llvm_runtime, &thread_pool,
                                          (void *)ThreadPool::static_run);

    runtime->call<void *, void *>("LLVMRuntime_set_assert_failed", llvm_runtime,
                                  (void *)assert_failed_host);
    // Profiler functions can only be called on host kernels
    runtime->call<void *, void *>("LLVMRuntime_set_profiler", llvm_runtime,
                                  profiler.get());
    runtime->call<void *, void *>("LLVMRuntime_set_profiler_start",
                                  llvm_runtime,
                                  (void *)&ProfilerBase::profiler_start);
    runtime->call<void *, void *>("LLVMRuntime_set_profiler_stop", llvm_runtime,
                                  (void *)&ProfilerBase::profiler_stop);
  }
}

void Program::materialize_layout() {
  // always use host_arch() this is for host accessors
  std::unique_ptr<StructCompiler> scomp =
      StructCompiler::make(this, host_arch());
  scomp->run(*snode_root, true);

  if (arch_is_cpu(config.arch)) {
    initialize_runtime_system(scomp.get());
  }

  TI_TRACE("materialize_layout called");
  if (config.arch == Arch::cuda) {
    initialize_device_llvm_context();
    std::unique_ptr<StructCompiler> scomp_gpu =
        StructCompiler::make(this, Arch::cuda);
    scomp_gpu->run(*snode_root, false);
    initialize_runtime_system(scomp_gpu.get());
  } else if (config.arch == Arch::metal) {
    TI_ASSERT_INFO(config.use_llvm,
                   "Metal arch requires that LLVM being enabled");
    metal_struct_compiled_ = metal::compile_structs(*snode_root);
    if (metal_runtime_ == nullptr) {
      metal::MetalRuntime::Params params;
      params.root_size = metal_struct_compiled_->root_size;
      params.config = &config;
      params.mem_pool = memory_pool.get();
      params.profiler = profiler.get();
      metal_runtime_ = std::make_unique<metal::MetalRuntime>(std::move(params));
    }
  } else if (config.arch == Arch::opengl) {
    opengl::OpenglStructCompiler scomp;
    opengl_struct_compiled_ = scomp.run(*snode_root);
    TI_INFO("OpenGL root buffer size: {} B",
            opengl_struct_compiled_->root_size);
    opengl::initialize_opengl();
  }
}

void Program::synchronize() {
  if (!sync) {
    if (config.arch == Arch::cuda) {
#if defined(TI_WITH_CUDA)
      cudaDeviceSynchronize();
#else
      TI_ERROR("No CUDA support");
#endif
    } else if (config.arch == Arch::metal) {
      metal_runtime_->synchronize();
    }
    sync = true;
  }
}

std::string capitalize_first(std::string s) {
  s[0] = std::toupper(s[0]);
  return s;
}

std::string latex_short_digit(int v) {
  std::string units = "KMGT";
  int unit_id = -1;
  while (v >= 1024 && unit_id + 1 < (int)units.size()) {
    TI_ASSERT(v % 1024 == 0);
    v /= 1024;
    unit_id++;
  }
  if (unit_id != -1)
    return fmt::format("{}\\mathrm{{{}}}", v, units[unit_id]);
  else
    return std::to_string(v);
}

void Program::visualize_layout(const std::string &fn) {
  {
    std::ofstream ofs(fn);
    TI_ASSERT(ofs);
    auto emit = [&](std::string str) { ofs << str; };

    auto header = R"(
\documentclass[tikz, border=16pt]{standalone}
\usepackage{latexsym}
\usepackage{tikz-qtree,tikz-qtree-compat,ulem}
\begin{document}
\begin{tikzpicture}[level distance=40pt]
\tikzset{level 1/.style={sibling distance=-5pt}}
  \tikzset{edge from parent/.style={draw,->,
    edge from parent path={(\tikzparentnode.south) -- +(0,-4pt) -| (\tikzchildnode)}}}
  \tikzset{every tree node/.style={align=center, font=\small}}
\Tree)";
    emit(header);

    std::function<void(SNode * snode)> visit = [&](SNode *snode) {
      emit("[.{");
      if (snode->type == SNodeType::place) {
        emit(snode->name);
      } else {
        emit("\\textbf{" + capitalize_first(snode_type_name(snode->type)) +
             "}");
      }

      std::string indices;
      for (int i = 0; i < taichi_max_num_indices; i++) {
        if (snode->extractors[i].active) {
          int nb = snode->extractors[i].num_bits;
          int start = snode->extractors[i].start + nb;
          indices += fmt::format(
              R"($\mathbf{{{}}}^{{\mathbf{{{}b}}:{}}}_{{\mathbf{{{}b}}:{}}}$)",
              std::string(1, 'I' + i), start, latex_short_digit(1 << start), nb,
              latex_short_digit(1 << nb));
        }
      }
      if (!indices.empty())
        emit("\\\\" + indices);
      if (snode->type == SNodeType::place) {
        emit("\\\\" + data_type_short_name(snode->dt));
      }
      emit("} ");

      for (int i = 0; i < (int)snode->ch.size(); i++) {
        visit(snode->ch[i].get());
      }
      emit("]");
    };

    visit(snode_root.get());

    auto tail = R"(
\end{tikzpicture}
\end{document}
)";
    emit(tail);
  }
  trash(system(fmt::format("pdflatex {}", fn).c_str()));
}

void Program::initialize_device_llvm_context() {
  if (config.arch == Arch::cuda) {
    if (llvm_context_device == nullptr)
      llvm_context_device = std::make_unique<TaichiLLVMContext>(Arch::cuda);
  }
}

Arch Program::get_snode_accessor_arch() {
  if (config.arch == Arch::opengl) {
    return Arch::opengl;
  } else if (config.arch == Arch::cuda && !config.use_unified_memory) {
    return Arch::cuda;
  } else if (config.arch == Arch::metal) {
    return Arch::metal;
  } else {
    return get_host_arch();
  }
}

Kernel &Program::get_snode_reader(SNode *snode) {
  TI_ASSERT(snode->type == SNodeType::place);
  auto kernel_name = fmt::format("snode_reader_{}", snode->id);
  auto &ker = kernel([&] {
    ExprGroup indices;
    for (int i = 0; i < snode->num_active_indices; i++) {
      indices.push_back(Expr::make<ArgLoadExpression>(i));
    }
    auto ret = Stmt::make<FrontendArgStoreStmt>(
        snode->num_active_indices, load_if_ptr((snode->expr)[indices]));
    current_ast_builder().insert(std::move(ret));
  });
  ker.set_arch(get_snode_accessor_arch());
  ker.name = kernel_name;
  ker.is_accessor = true;
  for (int i = 0; i < snode->num_active_indices; i++)
    ker.insert_arg(DataType::i32, false);
  auto ret_val = ker.insert_arg(snode->dt, false);
  ker.mark_arg_return_value(ret_val);
  return ker;
}

Kernel &Program::get_snode_writer(SNode *snode) {
  TI_ASSERT(snode->type == SNodeType::place);
  auto kernel_name = fmt::format("snode_writer_{}", snode->id);
  auto &ker = kernel([&] {
    ExprGroup indices;
    for (int i = 0; i < snode->num_active_indices; i++) {
      indices.push_back(Expr::make<ArgLoadExpression>(i));
    }
    (snode->expr)[indices] =
        Expr::make<ArgLoadExpression>(snode->num_active_indices);
  });
  ker.set_arch(get_snode_accessor_arch());
  ker.name = kernel_name;
  ker.is_accessor = true;
  for (int i = 0; i < snode->num_active_indices; i++)
    ker.insert_arg(DataType::i32, false);
  ker.insert_arg(snode->dt, false);
  return ker;
}

void Program::finalize() {
  if (runtime)
    runtime->set_profiler(nullptr);
  synchronize();
  current_program = nullptr;
  memory_pool->terminate();
#if defined(TI_WITH_CUDA)
  if (preallocated_device_buffer != nullptr)
    cudaFree(preallocated_device_buffer);
#endif
  finalized = true;
  num_instances -= 1;
}

Program::~Program() {
  if (!finalized)
    finalize();
}

TLANG_NAMESPACE_END
