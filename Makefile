#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)

APP = nvme_wrr_demo

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk
