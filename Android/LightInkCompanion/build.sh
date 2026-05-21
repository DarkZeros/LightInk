#!/bin/bash

# LightInk Companion APK Build Script
# Builds the Android app from command line without Android Studio
# Usage: ./build.sh [debug|release]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${1:-debug}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== LightInk Companion APK Builder ===${NC}"
echo "Build type: $BUILD_TYPE"
echo "Project directory: $SCRIPT_DIR"

# Check if gradlew exists
if [ ! -f "$SCRIPT_DIR/gradlew" ]; then
    echo -e "${RED}Error: gradlew not found. Please run this script from the project root.${NC}"
    exit 1
fi

# Check if gradle-wrapper.jar exists, download if missing
WRAPPER_JAR="$SCRIPT_DIR/gradle/wrapper/gradle-wrapper.jar"
if [ ! -f "$WRAPPER_JAR" ]; then
    echo -e "${YELLOW}gradle-wrapper.jar not found. Attempting to generate it...${NC}"

    # Option 1: Use system gradle to generate the wrapper jar
    if command -v gradle &>/dev/null; then
        echo -e "${YELLOW}Using system gradle to generate wrapper...${NC}"
        (cd "$SCRIPT_DIR" && gradle wrapper --gradle-version 8.14)
        if [ -f "$WRAPPER_JAR" ]; then
            echo -e "${GREEN}✓ gradle-wrapper.jar generated successfully${NC}"
        fi
    fi

    # Option 2: Download the Gradle distribution and build the wrapper jar from its components
    if [ ! -f "$WRAPPER_JAR" ]; then
        echo -e "${YELLOW}Downloading Gradle distribution to build wrapper jar...${NC}"

        # Parse distribution URL from wrapper properties
        DIST_URL=$(grep "^distributionUrl" "$SCRIPT_DIR/gradle/wrapper/gradle-wrapper.properties" \
            | cut -d'=' -f2- \
            | sed 's/\\:/:/g')

        if [ -z "$DIST_URL" ]; then
            echo -e "${RED}Error: Could not determine Gradle distribution URL from gradle-wrapper.properties${NC}"
            exit 1
        fi

        echo "Downloading from: $DIST_URL"
        TEMP_ZIP=$(mktemp --suffix=.zip)

        if ! curl -fsSL "$DIST_URL" -o "$TEMP_ZIP"; then
            echo -e "${RED}Error: Failed to download Gradle distribution${NC}"
            rm -f "$TEMP_ZIP"
            exit 1
        fi

        # Extract all wrapper jars from the distribution and combine into one
        TEMP_DIR=$(mktemp -d)
        unzip -q "$TEMP_ZIP" "*/lib/gradle-wrapper-*.jar" -d "$TEMP_DIR"

        # Find the extracted wrapper jars
        WRAPPER_JARS=$(find "$TEMP_DIR" -name "gradle-wrapper-*.jar" 2>/dev/null)

        if [ -z "$WRAPPER_JARS" ]; then
            echo -e "${RED}Error: Could not find wrapper jars in Gradle distribution${NC}"
            rm -rf "$TEMP_DIR" "$TEMP_ZIP"
            exit 1
        fi

        # Create a combined wrapper jar from all wrapper module jars
        COMBINED_DIR=$(mktemp -d)
        for jar in $WRAPPER_JARS; do
            (cd "$COMBINED_DIR" && jar xf "$jar")
        done

        (cd "$COMBINED_DIR" && jar cf "$WRAPPER_JAR" .)

        rm -rf "$TEMP_DIR" "$COMBINED_DIR" "$TEMP_ZIP"

        if [ -f "$WRAPPER_JAR" ]; then
            echo -e "${GREEN}✓ gradle-wrapper.jar built successfully${NC}"
        else
            echo -e "${RED}Error: Failed to build gradle-wrapper.jar${NC}"
            exit 1
        fi
    fi
fi

# Check if ANDROID_SDK_ROOT is set
if [ -z "$ANDROID_SDK_ROOT" ]; then
    echo -e "${YELLOW}Warning: ANDROID_SDK_ROOT not set. Trying to auto-detect...${NC}"
    
    # Try common Android SDK locations
    if [ -d "$HOME/Android/Sdk" ]; then
        export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
        echo -e "${GREEN}Found SDK at: $ANDROID_SDK_ROOT${NC}"
    elif [ -d "$HOME/Library/Android/sdk" ]; then
        export ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
        echo -e "${GREEN}Found SDK at: $ANDROID_SDK_ROOT${NC}"
    else
        echo -e "${RED}Error: Could not find Android SDK. Please set ANDROID_SDK_ROOT environment variable.${NC}"
        echo "Example: export ANDROID_SDK_ROOT=\$HOME/Android/Sdk"
        exit 1
    fi
fi

echo -e "${GREEN}Using Android SDK: $ANDROID_SDK_ROOT${NC}"

# Make gradlew executable
chmod +x "$SCRIPT_DIR/gradlew"

# Build based on type
case "$BUILD_TYPE" in
    debug)
        echo -e "${YELLOW}Building debug APK...${NC}"
        "$SCRIPT_DIR/gradlew" -p "$SCRIPT_DIR" clean assembleDebug
        APK_PATH="$SCRIPT_DIR/app/build/outputs/apk/debug/app-debug.apk"
        ;;
    release)
        echo -e "${YELLOW}Building release APK...${NC}"
        echo -e "${YELLOW}Note: Release builds require a signing key. Using debug key for now.${NC}"
        "$SCRIPT_DIR/gradlew" -p "$SCRIPT_DIR" clean assembleRelease
        APK_PATH="$SCRIPT_DIR/app/build/outputs/apk/release/app-release-unsigned.apk"
        ;;
    *)
        echo -e "${RED}Invalid build type: $BUILD_TYPE${NC}"
        echo "Usage: $0 [debug|release]"
        exit 1
        ;;
esac

# Check if build was successful
if [ -f "$APK_PATH" ]; then
    APK_SIZE=$(du -h "$APK_PATH" | cut -f1)
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo -e "${GREEN}APK: $APK_PATH${NC}"
    echo -e "${GREEN}Size: $APK_SIZE${NC}"
    
    # Show installation instructions
    echo ""
    echo -e "${YELLOW}To install on a connected device:${NC}"
    echo "  adb install -r \"$APK_PATH\""
    echo ""
    echo -e "${YELLOW}To install and run:${NC}"
    echo "  adb install -r \"$APK_PATH\" && adb shell am start -n com.lightink.companion/.MainActivity"
else
    echo -e "${RED}✗ Build failed!${NC}"
    exit 1
fi
