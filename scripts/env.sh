# Source inside the container: Symbian build environment.
export EPOCROOT=/work/build/epocroot/
export SBS_HOME=${EPOCROOT}linux_build/sbsv2/raptor
export SBS_GCCE441BIN=/work/build/toolchains/arm-2009q3/bin
export PATH="$SBS_HOME/bin:${EPOCROOT}epoc32/tools:$SBS_GCCE441BIN:$PATH"
export PYTHON=python2.6
export LOGS=/work/build/logs
mkdir -p "$LOGS"
