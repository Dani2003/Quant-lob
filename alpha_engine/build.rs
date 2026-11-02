fn main() {
    let mut build = cxx_build::bridge("src/lib.rs");

    build
        .file("src/order_book_bridge.cpp")
        .file("../src/order_book.cpp")
        .include("include")
        .include("../include");

    let target = std::env::var("TARGET").unwrap_or_default();

    if target.contains("msvc") {
        build
            .flag_if_supported("/std:c++20")
            .flag_if_supported("/O2")
            .flag_if_supported("/Oi")
            .flag_if_supported("/Ot");
    } else {
        build
            .flag_if_supported("-std=c++20")
            .flag_if_supported("-O3")
            .flag_if_supported("-march=native")
            .flag_if_supported("-Wall")
            .flag_if_supported("-Wextra")
            .flag_if_supported("-Wno-interference-size");
    }

    build.compile("quant_lob_bridge");

    println!("cargo:rustc-link-lib=static=quant_lob_bridge");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=include/order_book_bridge.hpp");
    println!("cargo:rerun-if-changed=src/order_book_bridge.cpp");
    println!("cargo:rerun-if-changed=../include/order_book.hpp");
    println!("cargo:rerun-if-changed=../include/memory_pool.hpp");
    println!("cargo:rerun-if-changed=../src/order_book.cpp");
}
