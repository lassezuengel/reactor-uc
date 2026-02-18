package org.lflang.generator.uc

import java.nio.file.Files
import java.nio.file.Path
import org.lflang.lf.Instantiation
import org.lflang.target.TargetConfig
import org.lflang.util.FileUtil

/**
 * Base class for optional platform-specific artifact generators. Subclasses emit additional build
 * scaffolding beyond the default native files and can take advantage of shared helpers.
 */
abstract class UcPlatformArtifactGenerator
protected constructor(
    protected val mainDef: Instantiation,
    protected val targetConfig: TargetConfig,
    protected val projectRoot: Path,
    protected val workspaceRoot: Path,
    protected val context: UcGeneratorFactory.PlatformContext
) {

  abstract fun generate()

  /**
   * Helper for copying files from the workspace to the generated project. This is useful for
   * platform-specific generators that want to allow users to provide custom files (e.g., additional
   * (overlay) configurations).
   */
  protected fun copyFromWorkspace(fileName: String) {
    val sourcePath = workspaceRoot.resolve(fileName)
    if (Files.exists(sourcePath)) {
      FileUtil.copyFile(sourcePath, projectRoot.resolve(fileName), false)
    }
  }
}
