// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production paged SPIR-V emitter and portable label writer on a real Vulkan GPU.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_gpu_labels.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

int main(int argc, char** argv) {
    using namespace Vulkan;
    using namespace Shader;
    // The regression must exercise no AMD marker commands, including diagnostic breadcrumbs.
    setenv("BB_BREADCRUMBS","0",1);
    Instance instance(0, true);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Scheduler scheduler(instance);
    constexpr u32 Page = 65536, Pages = 3, Bytes = Page * Pages;
    struct Allocation { VkBuffer buffer{}; VmaAllocation memory{}; void* mapped{}; u64 bda{}; };
    std::array<Allocation, 6> allocations{};
    for (auto& a : allocations) {
        const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=Bytes,
            .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO,
            .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo ai{};
        assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &a.buffer, &a.memory, &ai)==VK_SUCCESS);
        a.mapped=ai.pMappedData;
        a.bda=device.getBufferAddress({.buffer=a.buffer},d);
        std::memset(a.mapped,0,Bytes);
    }
    auto& guest=allocations[0]; auto& mirror=allocations[1]; auto& source=allocations[2];
    auto& records=allocations[3]; auto& table=allocations[4]; auto& trash=allocations[5];
    auto* host=static_cast<u32*>(guest.mapped);
    auto* vram=static_cast<u32*>(mirror.mapped);
    auto* input=static_cast<u32*>(source.mapped);
    input[0]=0x12345678; input[1]=0x23456789; input[2]=0x3456789a;
    // The source's current GPU view contains newer data than its guest view.
    input[3]=0x456789ab;
    constexpr u32 SrcPage=4, DstPage=8, GuestTable=16;
    auto* entries=static_cast<u64*>(table.mapped);
    entries[SrcPage]=source.bda;
    entries[SrcPage+GuestTable]=guest.bda+Page*2;
    entries[DstPage]=mirror.bda; entries[DstPage+GuestTable]=guest.bda;
    entries[DstPage+1]=guest.bda+Page; entries[DstPage+1+GuestTable]=guest.bda+Page;
    // DstPage+2 is unmapped even though the descriptor includes it.
    auto* record=static_cast<u32*>(records.mapped);
    const auto put_record=[&](u32 at,u32 base,u32 size,bool through) {
        const u32 words[8]={base,0,size,u32(through),u32(trash.bda),u32(trash.bda>>32),GuestTable,0};
        std::memcpy(record+at,words,sizeof(words));
    };
    put_record(0,SrcPage*Page,Page,false);
    put_record(64,DstPage*Page,Bytes,true);

    Info info{};
    info.hw_stage=HwStage::Compute; info.sw_stage=SwStage::Compute;
    info.pgm_hash=BufferCopyShaderHash; info.uses_paged_buffers=true;
    for (u32 i=0;i<3;++i) {
        BufferResource buffer{};
        buffer.buffer_type=i==2 ? BufferType::BdaPagetable : BufferType::Guest;
        buffer.used_types=i==2 ? IR::Type::U64 : IR::Type::U32;
        buffer.is_written=i==1;
        const auto sharp=AmdGpu::Buffer::Placeholder(64*1024*1024);
        std::memcpy(buffer.sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
        info.buffers.push_back(buffer);
    }
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool); IR::IREmitter ir(block);
    const std::array<u32,6> offsets{0,Page/4-1,Page/4,Page*2/4,Bytes/4,0xffffffffU/4};
    for (u32 i=0;i<offsets.size();++i) {
        const auto value=ir.LoadBufferU32(1,ir.Imm32(0U),ir.Imm32(i%4),{});
        ir.StoreBufferU32(1,ir.Imm32(1U),ir.Imm32(offsets[i]),value,{});
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage);
    runtime.hw.cs.workgroup_size={1,1,1};
    Profile profile{}; profile.supported_spirv=0x00010600; profile.support_int64=true;
    profile.paged_buffers=true; profile.sparse_page_shift=16;
    Backend::Bindings bindings{};
    const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    if (argc>1) {
        std::filesystem::create_directories(argv[1]);
        std::ofstream output(std::filesystem::path(argv[1])/"paged-copy.spv",std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32));
        assert(output);
    }
    std::array<vk::DescriptorSetLayoutBinding,3> desc{};
    for (u32 i=0;i<desc.size();++i) desc[i]={.binding=i,.descriptorType=vk::DescriptorType::eStorageBuffer,
        .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
    auto descriptor=device.createDescriptorSetLayoutUnique({
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=u32(desc.size()),.pBindings=desc.data()},nullptr,d).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,128};
    auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*descriptor,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,d).value;
    auto module=device.createShaderModuleUnique({.codeSize=code.size()*sizeof(u32),.pCode=code.data()},nullptr,d).value;
    auto pipeline=device.createComputePipelineUnique({}, {
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},
        .layout=*layout},nullptr,d).value;
    const auto dispatch=[&] {
        const auto command=scheduler.CommandBuffer();
        const std::array<vk::DescriptorBufferInfo,3> buffers{{
            {records.buffer,0,32},{records.buffer,256,32},{table.buffer,0,Bytes}}};
        std::array<vk::WriteDescriptorSet,3> writes{};
        for (u32 i=0;i<3;++i) writes[i]={.dstBinding=i,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[i]};
        command.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline,d);
        command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes,d);
        const std::array<u32,32> constants{};
        command.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,128,constants.data(),d);
        command.dispatch(1,1,1,d);
        // The production portable label publishes the shader's BDA writes to the host.
        RecordPortableGpuLabel(command,guest.buffer,Bytes-16,0x0123456789abcdefULL,8);
        scheduler.Finish();
    };
    dispatch();
    assert(host[0]==input[0] && vram[0]==input[0]);
    assert(host[Page/4-1]==input[1] && vram[Page/4-1]==input[1]);
    assert(host[Page/4]==input[2]); // in-place page: both views alias
    assert(host[Page*2/4]==0 && vram[Page*2/4]==0); // unmapped store was dropped
    assert(host[Bytes/4-3]==0x01234567 && host[Bytes/4-4]==0x89abcdef);
    std::puts("PASS paged copy: current source, mirror + guest stores, page boundary, unmapped/range guard");

    // A descriptor without write-through must not accidentally publish mirror-only data.
    record[64+3]=0; host[0]=0; vram[0]=0;
    dispatch();
    assert(host[0]==0 && vram[0]==input[0]);
    std::puts("PASS paged copy: write-through flag preserves mirror-only ownership");

    // Exercise ordering while the CPU polls, rather than only after waiting for a fence.
    constexpr u32 Iterations=512;
    host[0]=host[2]=host[3]=host[4]=0;
    auto command=scheduler.CommandBuffer();
    for (u32 i=1;i<=Iterations;++i) {
        command.fillBuffer(guest.buffer,0,4,i,d);
        RecordPortableGpuLabel(command,guest.buffer,8,u64(i)<<32|i,8);
        RecordPortableGpuLabel(command,guest.buffer,16,i,4);
    }
    command.copyBuffer(guest.buffer,mirror.buffer,vk::BufferCopy{8,8,12},d);
    RecordPortableGpuLabel(command,guest.buffer,24,0,8);
    scheduler.Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    u32 observed=0;
    while (observed<Iterations) {
        observed=__atomic_load_n(host+4,__ATOMIC_ACQUIRE);
        const auto payload=__atomic_load_n(host,__ATOMIC_ACQUIRE);
        assert(payload>=observed);
        const auto low=__atomic_load_n(host+2,__ATOMIC_ACQUIRE);
        const auto high=__atomic_load_n(host+3,__ATOMIC_ACQUIRE);
        assert(high>=low);
        assert(std::chrono::steady_clock::now()<deadline);
    }
    scheduler.Finish();
    assert(vram[2]==Iterations && vram[3]==Iterations && vram[4]==Iterations);
    assert(host[6]==0 && host[7]==0);
    std::puts("PASS portable labels: 32/64-bit, high-before-low, prior GPU payload visible during polling, GPU consumer, zero value");
    for (auto& a:allocations) vmaDestroyBuffer(instance.GetAllocator(),a.buffer,a.memory);
}
