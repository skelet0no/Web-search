# mongo-c 1.29's TRY_COMPILE passes a malformed CMake warning flag; CMake 4
# rejects it. Keep the intended compiler warning flag and a real link check.
file(READ "${SOURCE_DIR}/src/libmongoc/CMakeLists.txt" contents)
string(REPLACE "\"-Werror -DCMAKE_CXX_LINK_EXECUTABLE='echo not linking now...'\""
    "\"-DCMAKE_C_FLAGS=-Werror\"" contents "${contents}")
file(WRITE "${SOURCE_DIR}/src/libmongoc/CMakeLists.txt" "${contents}")
