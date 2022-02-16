serenity_component(
    VirGLDemo
    TARGETS VirGLDemo
)

set(SOURCES
    VirGLDemo.cpp
)

serenity_app(VirGLDemo ICON app-cube)
target_link_libraries(VirGLDemo LibMain)
