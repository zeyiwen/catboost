

LIBRARY()

NO_WERROR()

SRCS(
    auc.cpp
    gpu_metrics.cpp
    kernel/multilogit.cu
    kernel/pair_logit.cu
    kernel/pfound_f.cu
    kernel/pointwise_targets.cu
    kernel/query_cross_entropy.cu
    kernel/query_rmse.cu
    kernel/query_softmax.cu
    kernel/yeti_rank_pointwise.cu
    multiclass_targets.cpp
    pair_logit_pairwise.cpp
    pfound_f.cpp
    pointwise_target_impl.cpp
    query_cross_entropy.cpp
    querywise_targets_impl.cpp
    target_func.cpp
    GLOBAL kernel.cpp
    GLOBAL query_cross_entropy_kernels.cpp
    GLOBAL multiclass_kernels.cpp
)

PEERDIR(
    catboost/cuda/cuda_lib
    catboost/cuda/cuda_util
    catboost/cuda/gpu_data
    catboost/libs/helpers
    catboost/libs/metrics
    catboost/libs/options
)

INCLUDE(${ARCADIA_ROOT}/catboost/cuda/cuda_lib/default_nvcc_flags.make.inc)

END()
