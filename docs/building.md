# Building AIR Tools and Runtime

## Prerequisites

**mlir-air** is built and tested on Ubuntu 20.04 with the following packages installed:
```
  cmake 3.20.6
  clang/llvm 10+
  python 3.8.x
  ninja 1.10.0
  Xilinx Vitis 2021.2 
```

The minimum prerequisite tools and packages are:

```
clang/llvm 10+
python 3.8.x
pip3 install cmake ninja-build joblib psutil ipykernel numpy pybind11 PyYAML rich

```

In addition the following packages maybe useful: 

```

  Xilinx Vivado 2021.2/2022.1
  Xilinx Vitis 2021.2/2022.1
  Xilinx aienginev2 library from https://github.com/Xilinx/embeddedsw

```
Vivado and Vitis are used to build the platforms for available Xilinx cards with AIEs (VCK190 and VCK5000). Vitis also contains the AIE single core compiler xchesscc. The aienginev2 library is a driver backend used to configure the AIE array and used in building the AIR runtime.

Note that you do not need the above additional Xilinx packages to make use of the AIR compiler passes. 

Building mlir-air requires several other open source packages:
  - [mlir](https://github.com/llvm/llvm-project/tree/main/mlir)
  - [mlir-aie](https://github.com/Xilinx/mlir-aie)
  - [Xilinx cmakeModules](https://github.com/Xilinx/cmakeModules)

## Building external projects on X86

This mlir-air repository should already be cloned locally. 

First, clone and build LLVM, with the ability to target AArch64 as a cross-compiler, and with MLIR enabled. In addition, we make some common build optimizations to use a linker ('lld' or 'gold') other than 'ld' (which tends to be quite slow on large link jobs) and to link against libLLVM.so and libClang so. You may find that other options are also useful. Note that due to changing MLIR APIs, only a particular revision is expected to work.

To clone llvm and cmakeModules, see utils/clone-llvm.sh for the correct commithash. We point LLVM and subsequent tools to a common installation directory. 

```
cd utils
./clone-llvm.sh
./build-llvm-local.sh llvm build ../../install
```

Next, clone and build MLIR-AIE with absolute paths to the sysroot, llvm, and cmakeModules repositories. Again, we use a common installation directory.

```
git clone https://github.com/Xilinx/cmakeModules.git
./clone-mlir-aie.sh
./build-mlir-aie-local.sh $SYSROOT /full/path/to/mlir-air/utils/llvm /full/path/to/mlir-air/utils/cmakeModules mlir-aie build ../../install
```

The MLIR-AIE tools will be able to generate binaries targetting AIEngines.

Finally, build the MLIR-AIR tools for your desired use case: 

- Building on x86
- Building on x86 for deployment on a PCIe platform with AIEs
- Building on x86 for deployment on an embedded platform with AIEs
- Building on ARM for deployment on an embedded platform with AIEs

## Building on x86

Use the following command to build the AIR tools to compile on x86:

```
./build-mlir-air.sh $SYSROOT /full/path/to/mlir-air/utils/llvm /full/path/to/mlir-air/utils/cmakeModules /full/path/to/mlir-air/utils/mlir-aie ../../mlir-air build install
```

## Building on x86 with runtime for PCIe 

In order to build and run on PCIe cards, you first have to build and install the aienginev2 library:

```
git clone https://github.com/jgmelber/embeddedsw.git
cd embeddedsw
git checkout xlnx_rel_v2021.2-vck5000
cd XilinxProcessorIPLib/drivers/aienginev2/src
make -f Makefile.Linux
sudo cp -r ../include /opt/aiengine/
sudo cp libxaiengine.so* /opt/aiengine/lib/
export LD_LIBRARY_PATH=/opt/xaiengine/lib:${LD_LIBRARY_PATH}
```

Use the following command to build the AIR tools to compile on x86 for PCIe cards (VCK5000):

```
./build-mlir-air-pcie.sh /full/path/to/mlir-air/utils/llvm /full/path/to/mlir-air/utils/cmakeModules /full/path/to/mlir-air/utils/mlir-aie ../../mlir-air build install
```

## Environment setup

Set up your environment to use the tools you just built with the following commands:

```
export PATH=/path/to/mlir-air/install/bin:${PATH}
export PYTHONPATH=/path/to/mlir-air/install/python:${PYTHONPATH}
export LD_LIBRARY_PATH=/path/to/install/mlir-air/lib:/opt/xaiengine/lib:${LD_LIBRARY_PATH}
```

## Building hardware platforms

The instructions for building the hardware platform designs are found in the mlir-air/platforms directory:

- [xilinx_vck190_air](../platforms/xilinx_vck190_air)
- [xilinx_vck5000_air](../platforms/xilinx_vck5000_air)
- [xilinx_vck5000_air_scale_out](../platforms/xilinx_vck5000_air_scale_out)

## Building a Sysroot

Since the MLIR-AIE tools are cross-compiling, in order to actually compile code, we need a 'sysroot' directory,
containing an ARM rootfs.  This rootfs must match what will be available in the runtime environment.
Note that copying the rootfs is often insufficient, since many root file systems include absolute links.
Absolute symbolic links can be converted to relative symbolic links using [symlinks](https://github.com/brandt/symlinks).

```sh
cd /
sudo symlinks -rc .
```
Following the VCK190 platform build steps will also create a sysroot.

## Compiling Runtime for AArch64 (partial cross-compile)

Use the following command to build the AIR tools to build on x86 and cross-compile host code for ARM:

```
???
```

## Compiling Tools and Runtime for AArch64 (full cross-compile)

Use the following command to cross-compile the AIR tools for ARM:

```
cd utils
./cross-build-llvm.sh /full/path/to/mlir-air/install-aarch64
./cross-build-mlir-aie.sh ../cmake/modules/toolchain_crosscomp_aarch64.cmake $SYSROOT /full/path/to/mlir-air/utils/cmakeModules /full/path/to/mlir-air/utils/llvm /full/path/to/mlir-air/install-aarch64 
./cross-build-mlir-air.sh ../cmake/modules/toolchain_crosscomp_aarch64.cmake $SYSROOT /full/path/to/mlir-air/utils/cmakeModules /full/path/to/mlir-air/utils/llvm /full/path/to/mlir-air/utils/mlir-aie /full/path/to/mlir-air/install-aarch64
cd ..
tar -cvf air_tools.tar.gz install-aarch64
```

-----

<p align="center">Copyright&copy; 2022 Advanced Micro Devices, Inc.</p>
