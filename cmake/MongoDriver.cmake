include(ExternalProject)
function(tokenizer_fetch_mongo)
    set(prefix "${CMAKE_BINARY_DIR}/mongo")
    ExternalProject_Add(mongo_c_driver
        GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
        GIT_TAG 1.29.2 GIT_SHALLOW TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchMongoC.cmake
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${prefix} -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
            -DENABLE_SASL=OFF -DENABLE_STATIC=OFF -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF)
    ExternalProject_Add(mongo_cxx_driver DEPENDS mongo_c_driver
        GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
        GIT_TAG r3.11.0 GIT_SHALLOW TRUE
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${prefix} -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_PREFIX_PATH=${prefix} -DCMAKE_BUILD_TYPE=Release
            -DBUILD_VERSION=3.11.0 -DBUILD_TESTING=OFF -DENABLE_TESTS=OFF
            -DCMAKE_CXX_STANDARD=17 -DBSONCXX_POLY_USE_STD=ON
            -DENABLE_EXAMPLES=OFF -DENABLE_UNINSTALL=OFF
        BUILD_BYPRODUCTS "${prefix}/lib/libmongocxx.so" "${prefix}/lib/libbsoncxx.so")
    file(MAKE_DIRECTORY "${prefix}/include/mongocxx/v_noabi" "${prefix}/include/bsoncxx/v_noabi")
    foreach(lib mongocxx bsoncxx)
        add_library(mongo::${lib}_shared SHARED IMPORTED GLOBAL)
        set_target_properties(mongo::${lib}_shared PROPERTIES
            IMPORTED_LOCATION "${prefix}/lib/lib${lib}.so"
            INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include/${lib}/v_noabi")
        add_dependencies(mongo::${lib}_shared mongo_cxx_driver)
    endforeach()
    install(DIRECTORY "${prefix}/lib/" DESTINATION lib USE_SOURCE_PERMISSIONS)
    install(DIRECTORY "${prefix}/share/mongo-c-driver" "${prefix}/share/mongo-cxx-driver" DESTINATION share)
endfunction()
