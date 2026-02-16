package org.lflang.generator.uc

import java.nio.file.Path
import kotlin.math.max
import org.lflang.generator.ZephyrConfig
import org.lflang.lf.Instantiation
import org.lflang.target.TargetConfig
import org.lflang.target.property.FedNetInterfaceProperty
import org.lflang.target.property.PlatformProperty
import org.lflang.target.property.type.FedNetInterfaceType.FedNetInterface
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

  companion object {
    private var fedId = 0
  }

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
            |zephyr_compile_options(-Wno-error=unused-parameter -Wno-error=type-limits)
            |zephyr_compile_definitions(_GNU_SOURCE)
        """
                    .trimMargin(),
            projectName = projectName(),
            mainTargetName = "app",
            createMainTarget = false,
            additionalVariables = additionalVariables())
    FileUtil.writeToFile(cmake, projectRoot.resolve("CMakeLists.txt"))

    val prjConf =
        when (context) {
          is UcGeneratorFactory.PlatformContext.Standalone -> generatePrjConfStandalone()
          is UcGeneratorFactory.PlatformContext.Federated -> generatePrjConfFederated(context)
        }

    FileUtil.writeToFile(prjConf, projectRoot.resolve("prj.conf"))
  }

  /**
   * Generate a minimal prj.conf for standalone applications.
   *
   * TODO: Implement! We need only basic functionality, perhaps printk, no networking stuff.
   */
  private fun generatePrjConfStandalone(): String =
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
      """
          .trimMargin()

  /**
   * Generate a prj.conf for federated applications, enabling necessary Zephyr features for
   * communication and configuration. The generated config includes settings for POSIX sockets,
   * network buffers, IP address configuration, and additional system settings.
   */
  private fun generatePrjConfFederated(fed: UcGeneratorFactory.PlatformContext.Federated): String {
    val netInterface = targetConfig.get(FedNetInterfaceProperty.INSTANCE)
    val useIpv6 = netInterface == FedNetInterface.SICSLOWPAN

    if (!useIpv6) {
      return """
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
    }

    val devIpv6 =
      fed.federate.interfaces
        .asSequence()
        .filterIsInstance<UcTcpIpInterface>()
        .map { it.getIpAddress().address }
        .firstOrNull()
        ?: "fd00::${fedId++}"
    val maxConnections = maxConnectionsFor(fed).toString()

    return ZephyrConfig()
        .comment("Lingua Franca Zephyr configuration file")
        .comment("This is a generated file, do not edit.")
        .blank()
        .property("PRINTK", "y")
        .property("USE_SEGGER_RTT", "y")
        .property("LOG_BACKEND_RTT", "y")
        .property("DEBUG_INFO", "y")
        .property("RTT_CONSOLE", "y")
        .property("UART_CONSOLE", "n")
        .property("NET_LOG", "n")
        .property("LOG", "y")
        .heading("POSIX sockets and networking")
        .property("NETWORKING", "y")
        .property("NET_IPV6", "y")
        .property("NET_TCP", "y")
        .property("NET_SOCKETS", "y")
        .property("NET_CONNECTION_MANAGER", "y")
        .property("POSIX_API", "y")
        // .property("NET_SOCKETS_POSIX_NAMES", "y") // TODO: I don't think we need this in Zephyr 3.7.0, and Zephyr 4.1.0 doesn't support it at all!
        .heading("Network buffers")
        .property("NET_PKT_RX_COUNT", "16")
        .property("NET_PKT_TX_COUNT", "16")
        .property("NET_BUF_RX_COUNT", "64")
        .property("NET_BUF_TX_COUNT", "64")
        .property("NET_CONTEXT_NET_PKT_POOL", "y")
        .heading("IP address options")
        .property("NET_IF_UNICAST_IPV6_ADDR_COUNT", "3")
        .property("NET_IF_MCAST_IPV6_ADDR_COUNT", "4")
        .property("NET_MAX_CONTEXTS", "10")
        .heading("Network shell")
        .property("NET_SHELL", "y")
        .property("SHELL", "y")
        .heading("Network application options and configs")
        .property("NET_CONFIG_SETTINGS", "y")
        .property("NET_CONFIG_NEED_IPV4", "n")
        .property("NET_CONFIG_NEED_IPV6", "y")
        .property("NET_CONFIG_MY_IPV6_ADDR", "\"$devIpv6\"")
        .property("NET_MAX_CONN", maxConnections)
        .property("ZVFS_OPEN_MAX", "16")
        .property("NET_IF_MAX_IPV6_COUNT", "2")
        .heading("IEEE802.15.4 6LoWPAN")
        .property("BT", "n")
        .property("NET_UDP", "y")
        .property("NET_IPV4", "n")
        .property("NET_L2_IEEE802154_FRAGMENT_REASS_CACHE_SIZE", "8")
        .property("NET_CONFIG_MY_IPV4_ADDR", "\"\"")
        .property("NET_CONFIG_PEER_IPV4_ADDR", "\"\"")
        .property("NET_L2_IEEE802154", "y")
        .property("NET_L2_IEEE802154_SHELL", "y")
        .property("NET_IPV6_ND", "n")
        .property("NET_IPV6_NBR_CACHE", "n")
        .property("NET_CONFIG_IEEE802154_CHANNEL", "26")
        .heading("Additional system configuration")
        .property("SYSTEM_WORKQUEUE_STACK_SIZE", "4096")
        .property("MAIN_STACK_SIZE", "8192")
        .property("THREAD_CUSTOM_DATA", "y") // TODO: I don't think we need this
        .generateOutput()
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

  private fun maxConnectionsFor(fed: UcGeneratorFactory.PlatformContext.Federated): Int =
      max(1, UcConnectionGenerator.getNumNetworkBundles(fed.federate))

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
