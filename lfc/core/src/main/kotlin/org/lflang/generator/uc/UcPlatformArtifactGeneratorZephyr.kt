package org.lflang.generator.uc

import java.nio.file.Path
import org.lflang.lf.Instantiation
import org.lflang.target.TargetConfig
import org.lflang.target.property.PlatformProperty
import org.lflang.util.FileUtil

/**
 * Emits Zephyr-specific build scaffolding. The provided platform context distinguishes between
 * standalone applications and federated executables, allowing future federated-only extensions.
 */
class UcPlatformArtifactGeneratorZephyr(
    private val mainDef: Instantiation,
    private val targetConfig: TargetConfig,
    private val projectRoot: Path,
    private val context: UcGeneratorFactory.PlatformContext
) : UcPlatformArtifactGenerator {

  private val S = '$'

  override fun generate() {
    val platformOptions = targetConfig.get(PlatformProperty.INSTANCE)
    val boardFromTarget = platformOptions.board().value()
    val defaultBoard =
        boardFromTarget?.takeIf { platformOptions.board().setByUser() } ?: "nrf52840dk_nrf52840"
    val cmake =
        generateCmake(
            init =
                """
            |set(PLATFORM "ZEPHYR" CACHE STRING "Platform to target")
            |if (NOT DEFINED BOARD)
            |  set(BOARD "${defaultBoard}")
            |endif()
            |set(_LF_ZEPHYR_HINTS)
            |if(DEFINED ENV{ZEPHYR_BASE} AND EXISTS "${S}ENV{ZEPHYR_BASE}")
            |  list(APPEND _LF_ZEPHYR_HINTS "${S}ENV{ZEPHYR_BASE}")
            |endif()
            |if (EXISTS "${'$'}{CMAKE_CURRENT_SOURCE_DIR}/deps/zephyr")
            |  list(APPEND _LF_ZEPHYR_HINTS "${'$'}{CMAKE_CURRENT_SOURCE_DIR}/deps/zephyr")
            |endif()
            |if(_LF_ZEPHYR_HINTS)
            |  find_package(Zephyr REQUIRED HINTS ${'$'}{_LF_ZEPHYR_HINTS})
            |else()
            |  find_package(Zephyr REQUIRED)
            |endif()
            |zephyr_compile_options(-Wno-error=unused-parameter)
            |zephyr_compile_definitions(_GNU_SOURCE)
        """
                    .trimMargin(),
            projectName = projectName(),
            mainTargetName = "app",
            createMainTarget = false,
            additionalVariables = additionalVariables())
    FileUtil.writeToFile(cmake, projectRoot.resolve("CMakeLists.txt"))

    val prjConf =
        """
            |CONFIG_ETH_NATIVE_POSIX=n
            |CONFIG_NET_DRIVERS=y
            |CONFIG_NETWORKING=y
            |CONFIG_NET_TCP=y
            |CONFIG_NET_IPV4=y
            |CONFIG_NET_SOCKETS=y
            |CONFIG_POSIX_API=y
            |CONFIG_MAIN_STACK_SIZE=16384
            |CONFIG_HEAP_MEM_POOL_SIZE=1024
            |
            |# Network address config
            |CONFIG_NET_CONFIG_SETTINGS=y
            |CONFIG_NET_CONFIG_NEED_IPV4=y
            |CONFIG_NET_CONFIG_MY_IPV4_ADDR="127.0.0.1"
            |CONFIG_NET_SOCKETS_OFFLOAD=y
            |CONFIG_NET_NATIVE_OFFLOADED_SOCKETS=y
        """
            .trimMargin()
    FileUtil.writeToFile(prjConf, projectRoot.resolve("prj.conf"))
  }

  private fun projectName(): String =
      when (val ctx = context) {
        is UcGeneratorFactory.PlatformContext.Federated -> "${mainDef.name}_${ctx.federate.name}"
        UcGeneratorFactory.PlatformContext.Standalone -> mainDef.name
      }

  private fun additionalVariables(): List<String> =
      when (val ctx = context) {
        is UcGeneratorFactory.PlatformContext.Federated ->
            listOf("set(FEDERATE ${ctx.federate.name})")
        UcGeneratorFactory.PlatformContext.Standalone -> emptyList()
      }

  private fun generateCmake(
      init: String,
      projectName: String,
      mainTargetName: String,
      createMainTarget: Boolean,
      additionalVariables: List<String>
  ): String {
    val builder = StringBuilder()
    builder.appendLine("cmake_minimum_required(VERSION 3.20.0)")
    builder.appendLine(init)
    builder.appendLine("project(${projectName})")
    builder.appendLine("set(LF_MAIN ${mainDef.name})")
    builder.appendLine("set(LF_MAIN_TARGET ${mainTargetName})")
    builder.appendLine("set(PROJECT_ROOT ${projectRoot}/..)")
    additionalVariables.forEach { builder.appendLine(it) }
    builder.appendLine()
    if (createMainTarget) {
      builder.appendLine("add_executable(${S}{LF_MAIN_TARGET})")
    }
    builder.appendLine("include(${S}ENV{REACTOR_UC_PATH}/cmake/lfc.cmake)")
    builder.appendLine("lf_setup()")
    builder.appendLine(
        "lf_build_generated_code(${S}{LF_MAIN_TARGET} ${S}{CMAKE_CURRENT_SOURCE_DIR})")
    builder.appendLine("if (NOT TARGET ${S}{LF_MAIN_TARGET})")
    builder.appendLine(
        "  message(FATAL_ERROR \"Target ${S}{LF_MAIN_TARGET} was not created by Zephyr\")")
    builder.appendLine("endif()")
    builder.appendLine()
    return builder.toString()
  }
}
