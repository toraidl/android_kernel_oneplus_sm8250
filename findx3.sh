#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_DIR="$SCRIPT_DIR"
MAIN_DIR="$(dirname -- "$KERNEL_DIR")"
cd "$KERNEL_DIR"

KERNEL_DEFCONFIG="${KERNEL_DEFCONFIG:-vendor/kona-perf_defconfig}"
CLANG_DIR="${CLANG_DIR:-$MAIN_DIR/clang-10}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc --all)}"
RESUKISU_REPO_URL="${RESUKISU_REPO_URL:-https://github.com/ReSukiSU/ReSukiSU.git}"

BLUE='\033[0;34m'
NOCOL='\033[0m'
BUILD_FLAVOR="NOKSU"
RESUKISU_ENABLED=0
RESUKISU_REF="main"
RESUKISU_BACKUP_DIR=""
RESUKISU_LINK_STATE="absent"
RESUKISU_LINK_TARGET=""
RESUKISU_SOURCE_DIR=""
RESUKISU_COMMIT=""
PACKAGE_DIR=""
RESUKISU_LOG_PATCH="$KERNEL_DIR/findx3-resukisu-log-noise.diff"

usage() {
	cat <<EOF
Usage: $0 [resukisu [commit|tag]]

Build the Find X3 kernel and create an AnyKernel package.

  no argument                 Build the kernel without ReSukiSU.
  resukisu [commit|tag]       Integrate ReSukiSU and package the result.
                              Defaults to the ReSukiSU main branch.
  ksu [commit|tag]            Compatibility alias for resukisu.
  -h, --help                  Show this help.

Environment overrides:
  KERNEL_DEFCONFIG, CLANG_DIR, CROSS_COMPILE, JOBS
  RESUKISU_REPO_URL
EOF
}

die() {
	echo "findx3.sh: $*" >&2
	exit 1
}

cleanup_resukisu() {
	local rc=$?
	set +e

	if [[ -n "$RESUKISU_BACKUP_DIR" ]]; then
		cp -f -- "$RESUKISU_BACKUP_DIR/Makefile" drivers/Makefile
		cp -f -- "$RESUKISU_BACKUP_DIR/Kconfig" drivers/Kconfig

		case "$RESUKISU_LINK_STATE" in
		absent)
			if [[ -L drivers/kernelsu ]]; then
				rm -- drivers/kernelsu
			fi
			;;
		link)
			if [[ -L drivers/kernelsu ]]; then
				rm -- drivers/kernelsu
			fi
			ln -s -- "$RESUKISU_LINK_TARGET" drivers/kernelsu
			;;
		esac

		find "$RESUKISU_BACKUP_DIR" -depth -delete 2>/dev/null || true
		RESUKISU_BACKUP_DIR=""
		RESUKISU_SOURCE_DIR=""
	fi

	if [[ -n "$PACKAGE_DIR" ]]; then
		find "$PACKAGE_DIR" -depth -delete 2>/dev/null || true
		PACKAGE_DIR=""
	fi

	exit "$rc"
}
trap cleanup_resukisu EXIT

case "${1:-}" in
	"") ;;
	-h|--help)
		usage
		exit 0
		;;
	resukisu|ksu)
		if [[ $# -gt 2 ]]; then
			usage >&2
			exit 2
		fi
		RESUKISU_ENABLED=1
		BUILD_FLAVOR="RESUKISU"
		RESUKISU_REF="${2:-main}"
		if [[ "$1" == "ksu" ]]; then
			echo "findx3.sh: 'ksu' is kept as an alias; use 'resukisu' for ReSukiSU." >&2
		fi
		;;
	*)
		usage >&2
		exit 2
		;;
esac

if [[ ! -x "$CLANG_DIR/bin/clang" ]]; then
	mkdir -p -- "$CLANG_DIR"
	pushd "$CLANG_DIR" >/dev/null
	if [[ ! -s Clang-10-link.txt ]]; then
		curl -fsSL --retry 3 \
			-o Clang-10-link.txt \
			https://raw.githubusercontent.com/ZyCromerZ/Clang/main/Clang-10-link.txt
	fi
	clang_link="$(tr -d '\r\n' < Clang-10-link.txt)"
	[[ "$clang_link" == https://* ]] || die "invalid Clang archive URL"
	clang_archive="${clang_link##*/}"
	curl -fL --retry 3 -o "$clang_archive" "$clang_link"
	tar -xf "$clang_archive"
	popd >/dev/null
fi

export PATH="$CLANG_DIR/bin:$PATH"
export ARCH=arm64
export SUBARCH=arm64
export USE_CCACHE=1
export CONFIG_TECHPACK_CAMERA_OPPO=y
export KBUILD_COMPILER_STRING="$($CLANG_DIR/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')"

command -v curl >/dev/null || die "curl is required"
command -v git >/dev/null || die "git is required"
command -v 7za >/dev/null || die "7za is required for AnyKernel packaging"
[[ -x ./scripts/config ]] || die "scripts/config is missing or not executable"

prepare_resukisu() {
	local -a clone_args

	if ! git diff --quiet -- drivers/Makefile drivers/Kconfig; then
		die "drivers/Makefile or drivers/Kconfig has local changes; refusing to overwrite them"
	fi

	RESUKISU_BACKUP_DIR="$(mktemp -d /tmp/findx3-resukisu.XXXXXX)"
	cp -f -- drivers/Makefile "$RESUKISU_BACKUP_DIR/Makefile"
	cp -f -- drivers/Kconfig "$RESUKISU_BACKUP_DIR/Kconfig"

	if [[ -L drivers/kernelsu ]]; then
		RESUKISU_LINK_STATE="link"
		RESUKISU_LINK_TARGET="$(readlink -- drivers/kernelsu)"
	elif [[ -e drivers/kernelsu ]]; then
		die "drivers/kernelsu exists and is not a symlink"
	fi

	echo "**** Integrating ReSukiSU ref: $RESUKISU_REF ****"
	[[ "$RESUKISU_REPO_URL" == https://* ]] ||
		die "RESUKISU_REPO_URL must use HTTPS"
	RESUKISU_SOURCE_DIR="$RESUKISU_BACKUP_DIR/KernelSU"
	clone_args=(--quiet --no-checkout)
	if [[ -d KernelSU ]] && git -C KernelSU rev-parse --verify HEAD >/dev/null 2>&1; then
		clone_args+=(--reference-if-able="$KERNEL_DIR/KernelSU")
	fi
	git clone "${clone_args[@]}" "$RESUKISU_REPO_URL" "$RESUKISU_SOURCE_DIR" ||
		die "unable to clone ReSukiSU"
	git -C "$RESUKISU_SOURCE_DIR" checkout --quiet --detach "$RESUKISU_REF" ||
		die "ReSukiSU ref does not exist: $RESUKISU_REF"
	RESUKISU_COMMIT="$(git -C "$RESUKISU_SOURCE_DIR" rev-parse --verify 'HEAD^{commit}')" ||
		die "unable to determine the checked out ReSukiSU commit"

	ln -s -- "$(realpath --relative-to=drivers "$RESUKISU_SOURCE_DIR/kernel")" drivers/kernelsu
	printf '\nobj-$(CONFIG_KSU) += kernelsu/\n' >> drivers/Makefile
	sed -i '/endmenu/i\source "drivers/kernelsu/Kconfig"' drivers/Kconfig

	[[ -f "$RESUKISU_LOG_PATCH" ]] ||
		die "missing ReSukiSU log policy patch: $RESUKISU_LOG_PATCH"
	if ! git -C "$RESUKISU_SOURCE_DIR" apply --unidiff-zero --check "$RESUKISU_LOG_PATCH"; then
		die "ReSukiSU log policy patch does not apply to ref: $RESUKISU_REF"
	fi
	git -C "$RESUKISU_SOURCE_DIR" apply --unidiff-zero "$RESUKISU_LOG_PATCH" ||
		die "failed to apply ReSukiSU log policy patch"
	echo "ReSukiSU commit: $RESUKISU_COMMIT"
}

if (( RESUKISU_ENABLED )); then
	prepare_resukisu
fi

echo -e "$BLUE***********************************************"
echo "          BUILDING KERNEL ($BUILD_FLAVOR)"
echo -e "***********************************************$NOCOL"

make "$KERNEL_DEFCONFIG" O=out CC=clang
./scripts/config --file out/.config --enable STMVL53L1
if (( RESUKISU_ENABLED )); then
	./scripts/config --file out/.config --enable KSU --enable KSU_MANUAL_HOOK
fi
make olddefconfig O=out CC=clang

make -j"$JOBS" O=out \
	CC=clang \
	ARCH=arm64 \
	CROSS_COMPILE="$CROSS_COMPILE" \
	NM=llvm-nm \
	OBJDUMP=llvm-objdump \
	STRIP=llvm-strip

ZIMAGE_DIR="$KERNEL_DIR/out/arch/arm64/boot"
for artifact in Image dtbo.img dtb; do
	[[ -f "$ZIMAGE_DIR/$artifact" ]] || die "missing build artifact: $ZIMAGE_DIR/$artifact"
done

BUILD_TIME="$(date '+%Y%m%d-%H%M%S')"
PACKAGE="$KERNEL_DIR/BT-findx3-4.19.325-${BUILD_FLAVOR}-${BUILD_TIME}.zip"
PACKAGE_DIR="$(mktemp -d /tmp/findx3-package.XXXXXX)"
cp -f -- "$ZIMAGE_DIR/Image" "$PACKAGE_DIR/Image"
cp -f -- "$ZIMAGE_DIR/dtbo.img" "$PACKAGE_DIR/dtbo.img"
cp -f -- "$ZIMAGE_DIR/dtb" "$PACKAGE_DIR/dtb"
cp -a -- anykernel/. "$PACKAGE_DIR/"
if (( RESUKISU_ENABLED )); then
	printf 'repository=%s\nrequested_ref=%s\ncommit=%s\n' \
		"$RESUKISU_REPO_URL" "$RESUKISU_REF" "$RESUKISU_COMMIT" \
		> "$PACKAGE_DIR/resukisu-build-info.txt"
fi

(cd "$PACKAGE_DIR" && 7za a -mx9 "$PACKAGE" .)
echo "AnyKernel package: $PACKAGE"
