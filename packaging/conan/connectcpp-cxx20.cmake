# CMakeDeps-generated targets also carry the native package's language contract.
set_property(TARGET connectcpp::connectcpp APPEND PROPERTY
  INTERFACE_COMPILE_FEATURES cxx_std_20)
