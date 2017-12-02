#!/bin/bash
set -e

export ANDROID_NDK_ROOT=$ANDROID_NDK

VERSION=5_6_5
NAME=CRYPTOPP_${VERSION}
SHA512SUM=82e9a51080ace2734bfe4ba932c31e6a963cb20b570f0fea2fbe9ceccb887c8afecb36cde91c46ac6fea1357fdff6320ab2535b3f0aa48424acdd2cd9dd2e880

sDir=$(dirname $(readlink -f $0))
tDir=$(mktemp -d)
pushd $tDir

wget "https://github.com/weidai11/cryptopp/archive/$NAME.tar.gz"
echo "$SHA512SUM $NAME.tar.gz" | sha512sum --check -

tar -xf "$NAME.tar.gz"

pushd cryptopp-$NAME
make static > /dev/null
make install PREFIX=$sDir/linux

popd
rm -rf $tDir
