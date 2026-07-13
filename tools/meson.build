aegisub_mcp_stdio_src = files(
    'aegisub-mcp-stdio.cpp',
    '../libaegisub/common/cajun/elements.cpp',
    '../libaegisub/common/cajun/reader.cpp',
    '../libaegisub/common/cajun/writer.cpp',
)

aegisub_mcp_stdio_deps = []
aegisub_mcp_stdio_deps += boost_dep
if host_machine.system() == 'windows'
    aegisub_mcp_stdio_deps += cc.find_library('ws2_32', required: true)
endif

executable('aegisub-mcp-stdio',
           aegisub_mcp_stdio_src,
           include_directories: libaegisub_inc,
           dependencies: aegisub_mcp_stdio_deps,
           install: true,
           install_dir: bindir)
