#!/bin/bash

VERSION=$(grep -oP '(?<=VERSION ")[^"]*' ./settings.h);
DPATH="debian_package/asa_${VERSION}_amd64/"

# Make necessary directories
echo "Making directories...";
cd ../build;
mkdir -p ${DPATH}/opt;
mkdir -p ${DPATH}/usr/bin;
mkdir -p ${DPATH}/DEBIAN;

# Copy executable and necessary files
echo "Copying files...";
cp -r ./bin ${DPATH}/opt/asa;

# Create bin symlink
ln -f -s /opt/asa/asa ${DPATH}/usr/bin/asa;

# Create and clear control file
echo "Creating control file...";
touch ${DPATH}/DEBIAN/control;
echo "" > ${DPATH}/DEBIAN/control;

echo "Package: asa" >> ${DPATH}/DEBIAN/control;
echo "Version: ${VERSION}" >> ${DPATH}/DEBIAN/control;
echo "Architecture: amd64" >> ${DPATH}/DEBIAN/control;
echo "Maintainer: sam-astro <me@samuelhp.com>" >> ${DPATH}/DEBIAN/control;
echo "Description: The Asa programming language compiler" >> ${DPATH}/DEBIAN/control;

# Finally create deb
echo "Creating .DEB package...";
cd debian_package;
dpkg-deb --build --root-owner-group "../${DPATH}";


echo "DONE!";

