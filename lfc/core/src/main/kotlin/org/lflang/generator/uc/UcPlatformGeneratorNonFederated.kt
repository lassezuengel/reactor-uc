package org.lflang.generator.uc

import java.nio.file.Path
import org.lflang.reactor
import org.lflang.target.property.PlatformProperty
import org.lflang.target.property.type.PlatformType

class UcPlatformGeneratorNonFederated(generator: UcGenerator, override val srcGenPath: Path) :
    UcPlatformGenerator(generator) {

  override val buildPath = srcGenPath.resolve("build")
  override val nativeBuildTarget = fileConfig.name
  override val platform: PlatformType.Platform
    get() = targetConfig.get(PlatformProperty.INSTANCE).platform

  override fun generatePlatformFiles() {
    val numEventsAndReactions = generator.totalNumEventsAndReactions(generator.mainDef.reactor)

    // Use factory to create the appropriate platform-specific generators
    val mainGenerator =
        UcGeneratorFactory.createMainGenerator(
            mainReactor,
            generator.targetConfig,
            numEventsAndReactions.first,
            numEventsAndReactions.second,
            generator.fileConfig)

    val cmakeGenerator =
        UcGeneratorFactory.createCmakeGenerator(
            generator.mainDef, targetConfig, generator.fileConfig)

    val makeGenerator = UcMakeGeneratorNonFederated(mainReactor, targetConfig, generator.fileConfig)

    super.doGeneratePlatformFiles(mainGenerator, cmakeGenerator, makeGenerator)

    generatePlatformSpecificFiles(UcGeneratorFactory.PlatformContext.Standalone)
  }
}
