export TARGET_PLATFORM=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS6.0.sdk
export TARGET_BIN="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/usr/bin"
export TARGET_GCC=$TARGET_BIN/arm-apple-darwin10-llvm-gcc-4.2
export TARGET_CLANG=clang
export TARGET_LD=$TARGET_CLANG

export TARGET_CPPFLAGS="-I$TARGET_PLATFORM/usr/include/"
export TARGET_CFLAGS="-isysroot $TARGET_PLATFORM -march=armv7 -mcpu=cortex-a8 -mfpu=neon"
export TARGET_LDFLAGS="-L$TARGET_PLATFORM/usr/lib/"

export LOCAL_PLATFORM=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator6.0.sdk
export LOCAL_BIN="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/usr/bin"
export LOCAL_GCC=$LOCAL_BIN/i686-apple-darwin11-llvm-gcc-4.2
export LOCAL_LD=$TARGET_GCC
export LOCAL_CPPFLAGS="-I$LOCAL_PLATFORM/usr/include/"
export LOCAL_CFLAGS="-isysroot $LOCAL_PLATFORM -march=i386"
export LOCAL_LDFLAGS="-L$LOCAL_PLATFORM/usr/lib/"

sudo mkdir -p /Developer/usr/bin
sudo rm -f /Developer/usr/bin/ar
sudo ln -s /usr/bin/ar /Developer/usr/bin/ar

export PATH="`pwd`/bin:$PATH"
