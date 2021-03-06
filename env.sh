DEVELOPER_DIR=`xcode-select -print-path`
SDK_VERSION=${SDK_VERSION:-6.0}

case $SDK_VERSION in
	5.*) TARGET_DARWIN_VERSION=10 ; LOCAL_DARWIN_VERSION=10 ; GCC_VERSION=4.2 ;;
	6.*) TARGET_DARWIN_VERSION=10 ; LOCAL_DARWIN_VERSION=11 ; GCC_VERSION=4.2 ;;
esac

export TARGET_PLATFORM="$DEVELOPER_DIR/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS${SDK_VERSION}.sdk"
export TARGET_BIN="$DEVELOPER_DIR/Platforms/iPhoneOS.platform/Developer/usr/bin"
export TARGET_GCC=$TARGET_BIN/arm-apple-darwin${TARGET_DARWIN_VERSION}-llvm-gcc-${GCC_VERSION}
export TARGET_CLANG=clang
export TARGET_LD=$TARGET_CLANG

export TARGET_CPPFLAGS="-I$TARGET_PLATFORM/usr/include/"
export TARGET_CFLAGS="-isysroot $TARGET_PLATFORM -march=armv7 -mcpu=cortex-a8 -mfpu=neon"
export TARGET_LDFLAGS="-L$TARGET_PLATFORM/usr/lib/"

export LOCAL_PLATFORM="$DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator${SDK_VERSION}.sdk"
export LOCAL_BIN="$DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/usr/bin"
export LOCAL_GCC=$LOCAL_BIN/i686-apple-darwin${LOCAL_DARWIN_VERSION}-llvm-gcc-${GCC_VERSION}
export LOCAL_LD=$TARGET_GCC
export LOCAL_CPPFLAGS="-I$LOCAL_PLATFORM/usr/include/"
export LOCAL_CFLAGS="-isysroot $LOCAL_PLATFORM -march=i386"
export LOCAL_LDFLAGS="-L$LOCAL_PLATFORM/usr/lib/"

##sudo mkdir -p /Developer/usr/bin
##sudo rm -f /Developer/usr/bin/ar
##sudo ln -s /usr/bin/ar /Developer/usr/bin/ar

export PATH="`pwd`/bin:$PATH"
