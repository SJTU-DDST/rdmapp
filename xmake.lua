add_rules("mode.debug", "mode.release", "mode.releasedbg")

add_repositories("ddst-xrepo https://github.com/SJTU-DDST/xmake-repo.git")

option("docs", {default = false, description = "Build docs"})
option("asio_coro", {default = true, description = "Support Asio Coroutine"})
option("examples", {default = true, description = "Build examples"})
option("examples_pybind", {default = false, description = "Build pybind11 example"})
option("nortti", {default = true, description = "Build without RTTI"})
option("pic", {default = false, description = "Build with -fPIC for shared library"})


add_requires("ibverbs", { system = true })
add_requires("pthread", { system = true })
if has_config("asio_coro") then
    add_requires("asio 1.36.0", { private = true })
    add_defines("RDMAPP_ASIO_COROUTINE", { public = true })
end

if has_config("examples") then
    add_requires("spdlog 1.16.0", { private = true, configs = { header_only = true } })
    add_requires("cppcoro-20", { private = true })
    add_requires("concurrentqueue", { private = true })
end

-- helder functions
function has_pybind() 
    return has_config("examples") and has_config("examples_pybind")
end
function set_rtti()
    local enable = has_config("nortti") and not has_pybind()
    if enable then
        add_cxxflags("-fno-rtti", { public = true })
        add_defines("ASIO_NO_TYPEID", { public = true }) -- disable for asio
    end
end
function set_pic()
    if has_config("pic") or has_pybind() then
        add_cxxflags("-fpic", { public = true })
    end
end

if has_pybind() then
    add_requires("pybind11", { private = true })
    add_requires("python 3.x", { private = true })
end

set_languages("cxx20", { public = true })
set_warnings("all", "extra", "pedantic", "error", { private = true })

if is_mode("debug") then
    set_policy("build.sanitizer.address", true)
    add_defines("RDMAPP_BUILD_ASAN")
end

target("rdmapp")
    set_kind("static")
    add_files("src/*.cc")
    add_includedirs("include", { public = true })
    local source_path_len = #os.projectdir() + 1
    add_defines("SOURCE_PATH_LENGTH=" .. source_path_len)
    add_packages("ibverbs", "pthread", { public = true })
    if has_config("asio_coro") then
        add_packages("asio", { public = true })
    end
    if is_mode("debug") or is_mode("check") then
        add_defines("RDMAPP_BUILD_DEBUG", { public = true })
    end
    set_pic();
    set_rtti();
    add_headerfiles("include/(rdmapp/**.h)")
target_end()

if has_config("examples") then
    add_requires("asio 1.36.0", { private = true })
    target("rdmapp_examples_lib")
        set_rtti()
        set_pic()
        set_kind("static")
        add_deps("rdmapp", { public = true })
        add_packages("spdlog", "asio", { public = true })
        add_files("examples/qp_transmission.cc",
                  "examples/qp_acceptor.cc",
                  "examples/qp_connector.cc",
                  "examples/helloworld_handler.cc")
        add_includedirs("examples/include", { public = true })

    local examples = {"helloworld", "write_bw", "rpc", "cancellation", "latency", 
                      "latency_raw", "async_rpc", "send_bw_raw"}
    for _, name in ipairs(examples) do
        target(name)
            set_kind("binary")
            add_files("examples/" .. name .. ".cc")
            add_deps("rdmapp_examples_lib")
            add_packages("cppcoro-20", "concurrentqueue")
    end

    if has_config("examples_pybind") then
        target("rdmapp_py")
            set_kind("shared")
            set_filename("rdmapp_py.so") 
            add_deps("rdmapp_examples_lib")
            add_files("examples/pybind11/helloworld.cc")
            add_packages("pybind11", "python")
            add_deps("rdmapp")
            set_rtti()
            set_pic()
    end
end

task("doc")
    set_menu({usage = "xmake doc", description = "Generate API documentation with Doxygen"})
    on_run(function ()
        if os.exec("doxygen Doxyfile") == 0 then
            print("Doxygen build finished.")
        else
            print("Doxygen build failed!")
        end
    end)
