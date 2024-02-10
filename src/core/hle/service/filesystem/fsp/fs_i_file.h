// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/fsa/fs_i_file.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

class IFile final : public ServiceFramework<IFile> {
public:
    explicit IFile(Core::System& system_, FileSys::VirtualFile file_);

private:
    std::unique_ptr<FileSys::Fsa::IFile> backend;

    void Read(HLERequestContext& ctx);
    void Write(HLERequestContext& ctx);
    void Flush(HLERequestContext& ctx);
    void SetSize(HLERequestContext& ctx);
    void GetSize(HLERequestContext& ctx);
};

} // namespace Service::FileSystem
