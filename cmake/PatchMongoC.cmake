file(READ "${SOURCE_DIR}/src/libmongoc/CMakeLists.txt" contents)
string(REPLACE "\"-Werror -DCMAKE_CXX_LINK_EXECUTABLE='echo not linking now...'\""
    "\"-DCMAKE_C_FLAGS=-Werror\"" contents "${contents}")
file(WRITE "${SOURCE_DIR}/src/libmongoc/CMakeLists.txt" "${contents}")
