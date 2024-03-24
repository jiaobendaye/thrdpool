set_project(thrdpool)

set_languages("c99", "c++11")

add_cflags("-fPIC", "-pipe")
add_cxxflags("-fPIC", "-pipe", "-Wno-invalid-offsetof")

add_rules("mode.release", "mode.debug")

set_config("buildir", "build.xmake")
add_syslinks("pthread")

target("thrdpool")
    set_kind("static")
    add_files("*.c")
    add_cxflags("-fPIE")
    after_clean(function (target)
        os.rm("$(buildir)")
    end)

includes("example")
