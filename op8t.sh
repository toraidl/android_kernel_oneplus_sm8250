#!/bin/bash
#set -e
## Copy this script inside the kernel directory
LINKER="lld"
DIR=`readlink -f .`
MAIN=`readlink -f ${DIR}/..`
#KERNEL_DEFCONFIG=vendor/oplus-stock_defconfig
KERNEL_DEFCONFIG=vendor/kona-perf_defconfig
export USE_CCACHE=1

if [ ! -d "$MAIN/clang-10" ]; then
	mkdir "$MAIN/clang-10"
    pushd "$MAIN/clang-10" > /dev/null
	wget https://raw.githubusercontent.com/ZyCromerZ/Clang/main/Clang-10-link.txt
	clang_link="$(cat Clang-10-link.txt)"
    wget -q $clang_link
	tar -xf $(basename $clang_link)
	popd > /dev/null
fi
export PATH="$MAIN/clang-10/bin:$PATH"
export ARCH=arm64
export SUBARCH=arm64
export KBUILD_COMPILER_STRING="$($MAIN/clang-10/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')"


KERNEL_DIR=`pwd`
ZIMAGE_DIR="$KERNEL_DIR/out/arch/arm64/boot"
# Speed up build process
MAKE="./makeparallel"
BUILD_START=$(date +"%s")
blue='\033[0;34m'
cyan='\033[0;36m'
yellow='\033[0;33m'
red='\033[0;31m'
nocol='\033[0m'

if [[ $1 == "ksu" ]]; then 
	# kernel-SU add
    KSU_STATUS="ksu"
	curl -LSs "https://raw.githubusercontent.com/toraidl/KernelSU/legacy/kernel/setup.sh" | bash -s
	echo "**** Kernel defconfig is set to $KERNEL_DEFCONFIG ****"
else
    KSU_STATUS="noksu"
fi

echo -e "$blue***********************************************"
echo "          BUILDING KERNEL          "
echo -e "***********************************************$nocol"
make $KERNEL_DEFCONFIG O=out CC=clang
make -j$(nproc --all) O=out \
                      CC=clang \
                      ARCH=arm64 \
                      CROSS_COMPILE=aarch64-linux-gnu- \
                      NM=llvm-nm \
                      OBJDUMP=llvm-objdump \
                      STRIP=llvm-strip

TIME="$(date "+%Y%m%d-%H%M%S")"
mkdir -p tmp
cp -fp $ZIMAGE_DIR/Image tmp
cp -fp $ZIMAGE_DIR/dtbo.img tmp
cp -fp $ZIMAGE_DIR/dtb tmp
cp -rp ./anykernel/* tmp
cd tmp
7za a -mx9 tmp.zip *
cd ..
rm *.zip
cp -fp tmp/tmp.zip BT-op8t-4.19.157-$KSU_STATUS-stable-$TIME.zip
rm -rf tmp
echo $TIME

# Kernel-SU remove
git checkout drivers/Makefile &>/dev/null
git checkout drivers/Kconfig &>/dev/null
rm -rf KernelSU
rm -rf drivers/kernelsu

# dtsi change remove
# git checkout arch/arm64/boot/dts/vendor &>/dev/null
