if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(CMAKE_SKIP_RPATH TRUE)
    set(CMAKE_SKIP_BUILD_RPATH TRUE)
    set(CMAKE_SKIP_INSTALL_RPATH TRUE)
endif()

if(MSVC)
    set(SECURE_C_FLAGS "/GS")
    set(SECURE_CXX_FLAGS "/GS")
    set(SECURE_SHARED_LINKER_FLAGS "/NXCOMPAT /DYNAMICBASE")
    set(SECURE_EXE_LINKER_FLAGS "/NXCOMPAT /DYNAMICBASE")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(SECURE_SHARED_LINKER_FLAGS "/SAFESEH ${CMAKE_SHARED_LINKER_FLAGS}")
        set(SECURE_EXE_LINKER_FLAGS "/SAFESEH ${CMAKE_EXE_LINKER_FLAGS}")
    endif()
else()
    set(SECURE_C_FLAGS "-fPIC -fPIE -fstack-protector-strong")
    set(SECURE_CXX_FLAGS "-fPIC -fPIE -fstack-protector-strong")
    if(WIN32)
        set(SECURE_SHARED_LINKER_FLAGS "-Wl,--nxcompat -Wl,--dynamicbase -s")
        set(SECURE_EXE_LINKER_FLAGS "-Wl,--nxcompat -Wl,--dynamicbase -s")
        if(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(SECURE_SHARED_LINKER_FLAGS "-Wl,--no-seh ${SECURE_SHARED_LINKER_FLAGS}")
            set(SECURE_EXE_LINKER_FLAGS "-Wl,--no-seh ${SECURE_EXE_LINKER_FLAGS}")
        endif()
    else()
        set(SECURE_SHARED_LINKER_FLAGS "-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -s ")
        set(SECURE_EXE_LINKER_FLAGS "-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -s -pie")
        if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
            string(REPLACE "-s " "" SECURE_SHARED_LINKER_FLAGS "${SECURE_SHARED_LINKER_FLAGS}")
            string(REPLACE "-s " "" SECURE_EXE_LINKER_FLAGS "${SECURE_EXE_LINKER_FLAGS}")
        endif()
    endif()
endif()
