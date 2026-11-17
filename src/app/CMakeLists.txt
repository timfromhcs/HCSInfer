add_executable(hcsinfer main.cpp)
target_link_libraries(hcsinfer PRIVATE hcs_warnings hcs_cli hcs_core)
set_target_properties(hcsinfer PROPERTIES OUTPUT_NAME "HCSInfer")
