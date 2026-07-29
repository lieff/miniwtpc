
if [ "${ENABLE_ASAN}" = "yes" ]; then
  CFLAGS="${CFLAGS} -fsanitize=address -fsanitize-address-use-after-scope -fno-omit-frame-pointer -fno-common"
fi

if [ "${ENABLE_UBSAN}" = "yes" ]; then
  CFLAGS="${CFLAGS} -fsanitize=undefined -fno-omit-frame-pointer -fno-common"
fi

if [ "${ENABLE_RD}" = "yes" ]; then
  CFLAGS="${CFLAGS} -g"
fi

if [ "${ENABLE_DEBUG}" = "yes" ]; then
  CFLAGS="${CFLAGS} -g -O0 -D_DEBUG"
else
  CFLAGS="${CFLAGS} -O3 -ffunction-sections -fdata-sections -Wl,--gc-sections -U_FORTIFY_SOURCE -DNDEBUG"
fi

# Auto-detect x86 host and enable AVX unless explicitly disabled
if [ "${ENABLE_AVX}" = "no" ]; then
  :  # explicitly disabled
elif [ "${ENABLE_AVX}" = "yes" ] || [ "$(uname -m)" = "x86_64" ] || [ "$(uname -m)" = "i386" ] || [ "$(uname -m)" = "i686" ]; then
  CFLAGS="${CFLAGS} -mavx"
fi

gcc $CFLAGS -Wall -Wextra ./wtpc.c -o wtpc -lm -lpng16 -llcms2 -lpthread
