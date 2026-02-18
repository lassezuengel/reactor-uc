package org.lflang.generator.uc

import java.nio.file.Path
import org.lflang.generator.uc.espidf.UcEspIdfCmakeGenerator
import org.lflang.generator.uc.espidf.UcEspIdfMainGenerator
import org.lflang.generator.uc.freertos.UcFreeRtosMainGenerator
import org.lflang.lf.Instantiation
import org.lflang.lf.Reactor
import org.lflang.target.TargetConfig
import org.lflang.target.property.PlatformProperty
import org.lflang.target.property.type.PlatformType

/**
 * Factory for creating platform-specific generators (main, cmake, and make generators). This
 * centralizes the platform selection logic and makes it easy to add new platforms.
 *
 * Usage:
 * ```
 * val mainGen = UcGeneratorFactory.createMainGenerator(...)
 * val cmakeGen = UcGeneratorFactory.createCmakeGenerator(...)
 * ```
 */
object UcGeneratorFactory {

  /**
   * Creates the appropriate non-federated main generator based on the target platform
   * configuration.
   *
   * @param main The main reactor
   * @param targetConfig The target configuration containing platform information
   * @param numEvents Number of events in the system
   * @param numReactions Number of reactions in the system
   * @param fileConfig File configuration for path resolution
   * @return A platform-specific UcMainGeneratorNonFederated instance
   */
  fun createMainGenerator(
      main: Reactor,
      targetConfig: TargetConfig,
      numEvents: Int,
      numReactions: Int,
      fileConfig: UcFileConfig
  ): UcMainGeneratorNonFederated {
    val platform = targetConfig.get(PlatformProperty.INSTANCE).platform

    return when (platform) {
      PlatformType.Platform.FREERTOS ->
          UcFreeRtosMainGenerator(main, targetConfig, numEvents, numReactions, fileConfig)

      PlatformType.Platform.ESPIDF ->
          UcEspIdfMainGenerator(main, targetConfig, numEvents, numReactions, fileConfig)

      // Default case for POSIX, RP2040, ZEPHYR, RIOT, etc.
      else -> UcMainGeneratorNonFederated(main, targetConfig, numEvents, numReactions, fileConfig)
    }
  }

  /**
   * Creates the appropriate CMake generator based on the target platform configuration.
   *
   * @param mainDef The main instantiation
   * @param targetConfig The target configuration containing platform information
   * @param fileConfig File configuration for path resolution
   * @return A platform-specific UcCmakeGeneratorNonFederated instance
   */
  fun createCmakeGenerator(
      mainDef: Instantiation,
      targetConfig: TargetConfig,
      fileConfig: UcFileConfig
  ): UcCmakeGeneratorNonFederated {
    val platform = targetConfig.get(PlatformProperty.INSTANCE).platform

    return when (platform) {
      PlatformType.Platform.ESPIDF -> UcEspIdfCmakeGenerator(mainDef, targetConfig, fileConfig)

      // Default CMake generator for all other platforms
      else -> UcCmakeGeneratorNonFederated(mainDef, targetConfig, fileConfig)
    }
  }

  /** Describes the generator context so platform helpers can tailor their output. */
  sealed interface PlatformContext {
    /** Non-federated, single-application context. */
    data object Standalone : PlatformContext

    /** Federated application context carrying the current federate. */
    data class Federated(val federate: UcFederate) : PlatformContext
  }

  /**
   * Creates a platform-specific artifact generator (e.g., Zephyr CMake emitters) if needed. Returns
   * null when the platform uses only the standard native build files.
   *
   * @param mainDef The main instantiation
   * @param targetConfig The target configuration containing platform information
   * @param projectRoot The root directory of the generated project
   * @param workspaceRoot The root directory of the workspace, used for retrieving user-provided
   *   files
   * @param context The platform context (standalone vs federated) to tailor the generated artifacts
   */
  fun createPlatformArtifactGenerator(
      mainDef: Instantiation,
      targetConfig: TargetConfig,
      projectRoot: Path,
      workspaceRoot: Path,
      context: PlatformContext
  ): UcPlatformArtifactGenerator? {
    val platform = targetConfig.get(PlatformProperty.INSTANCE).platform

    return when (platform) {
      PlatformType.Platform.ZEPHYR ->
          UcPlatformArtifactGeneratorZephyr(
              mainDef, targetConfig, projectRoot, workspaceRoot, context)
      else -> null
    }
  }
}
