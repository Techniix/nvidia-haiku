// Stand-in for accelerant/NvPresent.cpp in the cursor test.
//
// Accelerant.cpp links against NvPresent for the GPU presentation hooks
// (B_GPU_PRESENT_HOOKS), which this test never asks for. The real file
// needs eventfd and NVKMS flip events, which the fake NVKMS doesn't
// provide, so the test links this instead. NvPresent's own behaviour is not
// covered here.

#include "NvPresent.h"

NvPresent::NvPresent(const Context &context): fContext(context) {}
NvPresent::~NvPresent() {}
void NvPresent::SetInstance(NvPresent *) {}
const gpu_present_hooks *NvPresent::Hooks() { return nullptr; }
