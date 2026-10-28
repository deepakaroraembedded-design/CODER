#!/usr/bin/env bash
# Run JaCoCo through Maven or Gradle and normalize the XML report for the
# gate's jacoco-xml parser.
#
# Usage: java-coverage.sh <src_dir> <build_dir>
set -euo pipefail

src_dir=$1
build_dir=$2
report="$build_dir/jacoco.xml"

if [[ -f "$src_dir/pom.xml" ]]; then
  (cd "$src_dir" && mvn -q test jacoco:report)
  source_report="$src_dir/target/site/jacoco/jacoco.xml"
elif [[ -f "$src_dir/build.gradle" || -f "$src_dir/build.gradle.kts" ]]; then
  (cd "$src_dir" && ./gradlew --no-daemon test jacocoTestReport)
  source_report="$src_dir/build/reports/jacoco/test/jacocoTestReport.xml"
else
  echo "coverage: no supported Java build (pom.xml or build.gradle)" >&2
  exit 2
fi

if [[ ! -f "$source_report" ]]; then
  echo "coverage: JaCoCo report not found at $source_report" >&2
  exit 1
fi
mkdir -p "$build_dir"
cp "$source_report" "$report"
echo "coverage report: $report"
