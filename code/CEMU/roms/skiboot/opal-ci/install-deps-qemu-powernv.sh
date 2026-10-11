#!/bin/bash
set -e
apt-get -y install eatmydata
eatmydata apt-get -y install gcc python g++ pkg-config libz-dev \
	libglib2.0-dev libpixman-1-dev libfdt-dev git ninja-build g++ \
	libbz2-dev sparse libglib2.0-dev make ccache
