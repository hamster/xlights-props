package com.xlightsprops.ddpdebugger;

/**
 * A separate entry point that does NOT extend {@link javafx.application.Application}.
 * When a fat jar's {@code Main-Class} extends Application directly, the plain JDK
 * {@code java -jar} launcher refuses to start it ("JavaFX runtime components are
 * missing") even though the JavaFX jars are right there on the classpath -- that check
 * only fires against the class actually named in the manifest. Routing through this
 * indirection avoids it.
 */
public final class Launcher {
    public static void main(String[] args) {
        Main.main(args);
    }
}
