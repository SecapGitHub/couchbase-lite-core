﻿//
// Document.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
using System;

using C4SequenceNumber = System.UInt64;

namespace LiteCore.Interop
{
    [Flags]
    public enum C4DocumentFlags : uint
    {
        Deleted = 0x01,
        Conflicted = 0x02,
        HasAttachments = 0x04,
        Exists = 0x1000
    }

    [Flags]
    public enum C4RevisionFlags : byte
    {
        None = 0x00,
        Deleted = 0x01,
        Leaf = 0x02,
        New = 0x04,
        HasAttachments = 0x08
    }

    public struct C4Revision
    {
        public C4Slice revID;
        public C4RevisionFlags flags;
        public C4SequenceNumber sequence;
        public C4Slice body;
    }

    public struct C4Document
    {
        public C4DocumentFlags flags;
        public C4Slice docID;
        public C4Slice revID;
        public C4SequenceNumber sequence;
        public C4Revision selectedRev;
    }

    public unsafe struct C4DocPutRequest
    {
        public C4Slice body;
        public C4Slice docID;
        public C4Slice docType;
        public C4RevisionFlags revFlags;
        private byte _existingRevision;
        private byte _allowConflict;
        public C4Slice* history;
        private UIntPtr _historyCount;
        private byte _save;
        public uint maxRevTreeDepth;
        public bool existingRevision
        {
            get {
                return Convert.ToBoolean(_existingRevision);
            }
            set {
                _existingRevision = Convert.ToByte(value);
            }
        }

        public bool allowConflict
        {
            get {
                return Convert.ToBoolean(_allowConflict);
            }
            set {
                _allowConflict = Convert.ToByte(value);
            }
        }

        public ulong historyCount
        {
            get {
                return _historyCount.ToUInt64();
            }
            set {
                _historyCount = (UIntPtr)value;
            }
        }

        public bool save
        {
            get {
                return Convert.ToBoolean(_save);
            }
            set {
                _save = Convert.ToByte(value);
            }
        }
    }
}
