package org.lflang.generator.uc

import java.nio.file.Path
import org.lflang.target.property.NoCompileProperty
import org.lflang.target.property.PlatformProperty
import org.lflang.target.property.type.PlatformType
import org.lflang.util.FileUtil

class UcPlatformGeneratorFederated(
    generator: UcGeneratorFederated,
    override val srcGenPath: Path,
    private val federate: UcFederate
) : UcPlatformGenerator(generator) {

  override val buildPath = srcGenPath.resolve("build")
  private val effectivePlatform: PlatformType.Platform
    get() =
        if (federate.platform == PlatformType.Platform.AUTO)
            targetConfig.get(PlatformProperty.INSTANCE).platform
        else federate.platform

  override val targetName: String =
      if (effectivePlatform == PlatformType.Platform.ZEPHYR) "zephyr" else federate.codeType

  override val buildTarget: String?
    get() = if (effectivePlatform == PlatformType.Platform.ZEPHYR) null else super.buildTarget

  override fun shouldCleanBuildDirectory(): Boolean =
      effectivePlatform == PlatformType.Platform.ZEPHYR

  override fun supportsInstallTarget(): Boolean = effectivePlatform != PlatformType.Platform.ZEPHYR

  override fun generatePlatformFiles() {
    val generator = generator as UcGeneratorFederated
    val numEventsAndReactions = generator.totalNumEventsAndReactionsFederated(federate)
    val mainGenerator =
        UcMainGeneratorFederated(
            federate,
            generator.federates,
            generator.targetConfig,
            numEventsAndReactions.first,
            numEventsAndReactions.second,
            generator.fileConfig)
    val cmakeGenerator = UcCmakeGeneratorFederated(federate, targetConfig, generator.fileConfig)
    val makeGenerator = UcMakeGeneratorFederated(federate, targetConfig, generator.fileConfig)
    val shouldGenerateNativeBuildFiles = effectivePlatform != PlatformType.Platform.ZEPHYR
    super.doGeneratePlatformFiles(
        mainGenerator, cmakeGenerator, makeGenerator, shouldGenerateNativeBuildFiles)

    generatePlatformSpecificFiles(UcGeneratorFactory.PlatformContext.Federated(federate))

    if (targetConfig.get(PlatformProperty.INSTANCE).platform == PlatformType.Platform.NATIVE &&
        !targetConfig.get(NoCompileProperty.INSTANCE)) {
      messageReporter.nowhere().info("Generating launch script for native federation.")
      generateLaunchScript()
    }
  }

  fun generateLaunchScript() {
    val generator = generator as UcGeneratorFederated
    val launchScriptGenerator = UcFederatedLaunchScriptGenerator(fileConfig)
    FileUtil.writeToFile(
        launchScriptGenerator.generateLaunchScript(generator.federates),
        fileConfig.binPath.resolve(fileConfig.name))
    fileConfig.binPath.resolve(fileConfig.name).toFile().setExecutable(true)
  }
}
