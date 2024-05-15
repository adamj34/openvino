// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "paged_attn.h"

#include "common/arbitrary_order_desc_creator.h"
#include "common/primitive_hashing_utils.hpp"
#include "cpu/x64/cpu_isa_traits.hpp"
#include "dnnl_extension_utils.h"
#include "memory_desc/cpu_memory_desc_utils.h"
#include "memory_desc/dnnl_blocked_memory_desc.h"
#include "onednn/dnnl.h"
#include "openvino/core/parallel.hpp"
#include "openvino/util/common_util.hpp"
#include "shape_inference/custom/paged_attn.hpp"
#include "shape_inference/shape_inference_internal_dyn.hpp"

#include "utils/plain_tensor.hpp"
#include "kernels/scaled_attn/executor_pa.hpp"
#include "kernels/scaled_attn/attn_memcpy.hpp"
#include "kernels/scaled_attn/attn_quant.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace ov::Extensions::Cpu;
using namespace ov::Extensions::Cpu::XARCH;
using namespace dnnl::impl;
using namespace dnnl::impl::cpu::x64;

namespace ov {
namespace intel_cpu {
namespace node {

struct PagedAttentionKey {
    ov::element::Type rtPrecision;

    size_t hash() const;
    bool operator==(const PagedAttentionKey& rhs) const;
};

size_t PagedAttentionKey::hash() const {
    size_t seed = 0;
    seed = hash_combine(seed, rtPrecision.hash());

    return seed;
}

bool PagedAttentionKey::operator==(const PagedAttentionKey& rhs) const {
    auto retVal = rtPrecision == rhs.rtPrecision;

    return retVal;
}

PagedAttention::PagedAttention(const std::shared_ptr<ov::Node>& op, const GraphContext::CPtr context)
    : Node(op, context, PAShapeInferFactory(op)) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        OPENVINO_THROW("CPU: " + errorMessage);
    }
}

void PagedAttention::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;
    auto rtPrecision = getRuntimePrecision();

    NodeConfig config;
    auto& creatorsMap = BlockedDescCreator::getCommonCreators();
    auto orgInputNumber = getOriginalInputsNumber();
    config.inConfs.resize(orgInputNumber);
    config.outConfs.resize(getOriginalOutputsNumber());
    config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        rtPrecision, getInputShapeAtPort(0)));
    config.inConfs[1].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        rtPrecision, getInputShapeAtPort(1)));
    config.inConfs[2].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        rtPrecision, getInputShapeAtPort(2)));

    OPENVINO_ASSERT(orgInputNumber == 13 || orgInputNumber == 14, "The input number of PagedAttention should be 13 or 14.");
    // kvcache, float, []
    auto past_kv_input_mem_precision = getOriginalInputPrecisionAtPort(PagedAttentionExecutor::ID_KCACHE);
    config.inConfs[PagedAttentionExecutor::ID_KCACHE].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        past_kv_input_mem_precision, getInputShapeAtPort(PagedAttentionExecutor::ID_KCACHE)));
    config.inConfs[PagedAttentionExecutor::ID_VCACHE].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        past_kv_input_mem_precision, getInputShapeAtPort(PagedAttentionExecutor::ID_VCACHE)));
    // is_prompt, bool, []
    config.inConfs[PagedAttentionExecutor::ID_IS_PROMPT].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::u8, getInputShapeAtPort(PagedAttentionExecutor::ID_IS_PROMPT)));
    // slot_mapping, int, [batch_size, max_context_len]
    config.inConfs[PagedAttentionExecutor::ID_SLOT_MAPPING].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_SLOT_MAPPING)));
    // max_context_len, int, []
    config.inConfs[PagedAttentionExecutor::ID_MAX_CONTEXT_LEN].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_MAX_CONTEXT_LEN)));
    // context_lens, int, [batch_size]
    config.inConfs[PagedAttentionExecutor::ID_CONTEXT_LENS].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_CONTEXT_LENS)));
    // block_tables, int, [batch_size, max_block_per_request]
    config.inConfs[PagedAttentionExecutor::ID_BLOCK_TABLES].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_BLOCK_TABLES)));
    // scale, float, []
    config.inConfs[PagedAttentionExecutor::ID_SCALE].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::f32, getInputShapeAtPort(PagedAttentionExecutor::ID_SCALE)));
    // alibi_slopes, float, [?] or nullptr
    config.inConfs[PagedAttentionExecutor::ID_ALIBI_SLOPES].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::f32, getInputShapeAtPort(PagedAttentionExecutor::ID_ALIBI_SLOPES)));
    // sliding_window, int, []
    config.inConfs[PagedAttentionExecutor::ID_SLIDING_WINDOW].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_SLIDING_WINDOW)));
    if (orgInputNumber == 14) {
        // subsequence_lens, int, [batch_size]
        config.inConfs[PagedAttentionExecutor::ID_SUBSEQUENCE_LENS].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
            ov::element::i32, getInputShapeAtPort(PagedAttentionExecutor::ID_SUBSEQUENCE_LENS)));
    }

    config.outConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
        rtPrecision, getOutputShapeAtPort(0)));

    supportedPrimitiveDescriptors.emplace_back(config, impl_desc_type::ref_any);
}

void PagedAttention::createPrimitive() {
    auto rtPrecision = getRuntimePrecision();

    // in one model, kvCachePrecision could not be changed so no need to care whether it may be changed.
    PagedAttentionKey key = {rtPrecision};

    auto builder = [&](const PagedAttentionKey& key) -> std::shared_ptr<PagedAttentionExecutor> {
#ifdef OPENVINO_ARCH_X86_64
        auto kvCachePrecision = getOriginalInputPrecisionAtPort(PagedAttentionExecutor::ID_KCACHE);
        return make_pa_executor(rtPrecision, kvCachePrecision);
#else
        return nullptr;
#endif
    };

    auto cache = context->getParamsCache();
    auto result = cache->getOrCreate(key, builder);
    if (!result.first) {
        OPENVINO_THROW("PagedAttention AttentionExecutor creation fails with precision " + rtPrecision.to_string());
    }
    m_executor = result.first;
}

void PagedAttention::execute(dnnl::stream strm) {
    auto orginInputNumber = getOriginalInputsNumber();
    std::vector<MemoryPtr> inputs(orginInputNumber);
    auto output = getDstMemoryAtPort(0);
    for (size_t i = 0; i < orginInputNumber; i++) {
        inputs[i] = getSrcMemoryAtPort(i);
    }

    gatherConcatPastkvForPagedAttn(inputs);

    m_executor->execute(inputs, output);
}

bool PagedAttention::isSupportedOperation(const std::shared_ptr<const ov::Node>& op, std::string& errorMessage) noexcept {
    try {
        int orgInput = static_cast<int>(op->get_input_size());
        if (op->get_type_name() == std::string("PagedAttentionExtension") && orgInput == PagedAttentionExecutor::ID_SLIDING_WINDOW + 1) {
            return true;
        }
    } catch (...) {
        return false;
    }
    return true;
}

void PagedAttention::gatherConcatPastkvForPagedAttn(const std::vector<MemoryPtr>& inputs) {
    PlainTensor k, v, k_cache, v_cache, slot_mapping;

    k.reset(inputs[PagedAttentionExecutor::ID_K]);                          // [B, L1, H * S]
    v.reset(inputs[PagedAttentionExecutor::ID_V]);
    k_cache.reset(inputs[PagedAttentionExecutor::ID_KCACHE]);               // [NUM_BLOCKS, H, 32, S]
    v_cache.reset(inputs[PagedAttentionExecutor::ID_VCACHE]);               // [NUM_BLOCKS, H, 32, S]
    slot_mapping.reset(inputs[PagedAttentionExecutor::ID_SLOT_MAPPING]);    // [B, max_context_len]

    auto B = k.size(0);
    auto L1 = k.size(1);
    auto H = k_cache.size(1);
    auto S = v_cache.size(3) - (k_cache.m_dt == ov::element::Type_t::u8 ? 8 : 0);

    k.assert_dims({B, L1, H * S});
    v.assert_dims({B, L1, H * S});
    slot_mapping.assert_dims({B, 0}, true);
    k = k.reshape({B, L1, H, S}).permute({0, 2, 1, 3});
    v = v.reshape({B, L1, H, S}).permute({0, 2, 1, 3});
    if (k_cache.m_dt == ov::element::Type_t::u8) {
        k_cache.assert_dims({0, H, 0, S + 8}, true);
        v_cache.assert_dims({k_cache.m_dims[0], H, k_cache.m_dims[2], S + 8});
        paged_attn_quantkv(k, v, k_cache, v_cache, slot_mapping);
    } else {
        k_cache.assert_dims({0, H, 0, S}, true);
        v_cache.assert_dims({k_cache.m_dims[0], H, k_cache.m_dims[2], S});
        paged_attn_memcpy(k, v, k_cache, v_cache, slot_mapping);
    }
}

ov::element::Type PagedAttention::getRuntimePrecision() const {
    auto rtPrecision = getOriginalInputPrecisionAtPort(0);
    // bf16 should be enabled only when platform supports
    if (rtPrecision == ov::element::bf16 && ov::with_cpu_x86_bfloat16()) {
        rtPrecision = ov::element::bf16;
    } else {
        rtPrecision = ov::element::f32;
    }
    return rtPrecision;
}

}  // namespace node
}  // namespace intel_cpu
}  // namespace ov
