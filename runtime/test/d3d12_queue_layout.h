// The queue whose handle a test holds is either a real PC ID3D12CommandQueue or the Xbox-ABI queue proxy
// (runtime/shims/d3d12_xs_device.h). The proxy speaks the console's newer queue layout (game_scarlett_master.exe;
// console_slot_map §3): two console methods at 14/15 push the PC's Signal/Wait to 16/17,
// GetTimestampFrequency/GetClockCalibration to 18/19 and GetDesc to 20 (28 in one source). A real queue keeps the PC
// order. These helpers call whichever slot the queue in hand uses, so a test sequence can run on both without
// knowing which it got; a real queue always goes through the PC interface and keeps the exact PC behaviour.
//
// queue_proxy_test.cpp has the same helpers inline (its own copies, next to the raw-call scaffolding they share).
#pragma once
#include <windows.h>
#include <d3d12.h>

#include <cstdint>

#include "../shims/d3d12_xs_device.h"

namespace queuelayout {

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

// A raw call through slot `slot` with the given words (the rest zero), the way the console's layout is addressed.
inline std::uint64_t raw_call(void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(object, a1, a2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

inline HRESULT as_hr(std::uint64_t value) { return static_cast<HRESULT>(static_cast<std::uint32_t>(value)); }

inline HRESULT queue_signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    if (!rt::d3d12_xs::is_queue_proxy(queue)) return queue->Signal(fence, value);
    return as_hr(raw_call(queue, 16, reinterpret_cast<std::uint64_t>(fence), value));
}

inline HRESULT queue_wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    if (!rt::d3d12_xs::is_queue_proxy(queue)) return queue->Wait(fence, value);
    return as_hr(raw_call(queue, 17, reinterpret_cast<std::uint64_t>(fence), value));
}

inline HRESULT queue_frequency(ID3D12CommandQueue* queue, UINT64* frequency) {
    if (!rt::d3d12_xs::is_queue_proxy(queue)) return queue->GetTimestampFrequency(frequency);
    return as_hr(raw_call(queue, 18, reinterpret_cast<std::uint64_t>(frequency)));
}

inline HRESULT queue_calibration(ID3D12CommandQueue* queue, UINT64* gpu, UINT64* cpu) {
    if (!rt::d3d12_xs::is_queue_proxy(queue)) return queue->GetClockCalibration(gpu, cpu);
    return as_hr(raw_call(queue, 19, reinterpret_cast<std::uint64_t>(gpu), reinterpret_cast<std::uint64_t>(cpu)));
}

// GetDesc is a struct return: the hidden pointer is the first declared argument (RCX = this, RDX = the buffer).
inline D3D12_COMMAND_QUEUE_DESC queue_description(ID3D12CommandQueue* queue) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    if (!rt::d3d12_xs::is_queue_proxy(queue)) return queue->GetDesc();
    raw_call(queue, 20, reinterpret_cast<std::uint64_t>(&desc));
    return desc;
}

}  // namespace queuelayout
