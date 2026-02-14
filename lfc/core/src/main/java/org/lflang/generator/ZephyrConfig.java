package org.lflang.generator;

import java.util.ArrayList;
import java.util.List;

/**
 * A simple ADT for building Zephyr configuration files programmatically.
 */
public class ZephyrConfig {

  // Algebraic data type for all supported config elements.
  private sealed interface ConfigElement permits Comment, BlankLine, PropertyAssignment {
  }

  private record Comment(String text) implements ConfigElement {
  }

  private record BlankLine() implements ConfigElement {
  }

  private record PropertyAssignment(String key, String value) implements ConfigElement {
  }

  private final List<ConfigElement> elements = new ArrayList<>();

  /**
   * Add a config entry.
   */
  public ZephyrConfig property(String key, String value) {
    elements.add(new PropertyAssignment(key, value));
    return this;
  }

  /**
   * Add a config entry if the condition is true.
   */
  public ZephyrConfig property_if(boolean condition, String key, String value) {
    if (condition) {
      property(key, value);
    }
    return this;
  }

  /**
   * Add a blank line to the config.
   */
  public ZephyrConfig blank() {
    elements.add(new BlankLine());
    return this;
  }

  /**
   * Add a comment line.
   */
  public ZephyrConfig comment(String comment) {
    elements.add(new Comment(comment));
    return this;
  }

  /**
   * Add a heading section (formatted comment).
   */
  public ZephyrConfig heading(String heading) {
    elements.add(new BlankLine());
    elements.add(new Comment(heading + " #"));
    elements.add(new Comment("-".repeat(heading.length()) + " #"));
    elements.add(new BlankLine());
    return this;
  }

  /**
   * Generate the output string for this config.
   * This can be written directly to a zephyr project config file.
   */
  public String generateOutput() {
    StringBuilder sb = new StringBuilder();

    for (ConfigElement e : this.elements) {
      if (e instanceof Comment c) {
        sb.append("# ").append(c.text()).append("\n");
      } else if (e instanceof BlankLine) {
        sb.append("\n");
      } else if (e instanceof PropertyAssignment p) {
        sb.append("CONFIG_").append(p.key()).append("=").append(p.value()).append("\n");
      }
    }

    return sb.toString();
  }
}
