/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "reference_table.h"

#include "class_linker.h"
#include "common_runtime_test.h"
#include "handle_scope-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/string.h"
#include "primitive.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

namespace art {

class ReferenceTableTest : public CommonRuntimeTest {};

static mirror::Object* CreateWeakReference(mirror::Object* referent)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Thread* self = Thread::Current();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  StackHandleScope<3> scope(self);
  Handle<mirror::Object> h_referent(scope.NewHandle<mirror::Object>(referent));

  Handle<mirror::Class> h_ref_class(scope.NewHandle<mirror::Class>(
      class_linker->FindClass(self,
                              "Ljava/lang/ref/WeakReference;",
                              ScopedNullHandle<mirror::ClassLoader>())));
  CHECK(h_ref_class.Get() != nullptr);
  CHECK(class_linker->EnsureInitialized(self, h_ref_class, true, true));

  Handle<mirror::Object> h_ref_instance(scope.NewHandle<mirror::Object>(
      h_ref_class->AllocObject(self)));
  CHECK(h_ref_instance.Get() != nullptr);

  ArtMethod* constructor = h_ref_class->FindDeclaredDirectMethod(
      "<init>", "(Ljava/lang/Object;)V", class_linker->GetImagePointerSize());
  CHECK(constructor != nullptr);

  uint32_t args[2];
  args[0] = PointerToLowMemUInt32(h_ref_instance.Get());
  args[1] = PointerToLowMemUInt32(h_referent.Get());
  JValue result;
  constructor->Invoke(self, args, sizeof(uint32_t), &result, constructor->GetShorty());
  CHECK(!self->IsExceptionPending());

  return h_ref_instance.Get();
}

TEST_F(ReferenceTableTest, Basics) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Object* o1 = mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello");

  ReferenceTable rt("test", 0, 11);

  // Check dumping the empty table.
  {
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("(empty)"), std::string::npos) << oss.str();
    EXPECT_EQ(0U, rt.Size());
  }

  // Check removal of all nullss in a empty table is a no-op.
  rt.Remove(nullptr);
  EXPECT_EQ(0U, rt.Size());

  // Check removal of all o1 in a empty table is a no-op.
  rt.Remove(o1);
  EXPECT_EQ(0U, rt.Size());

  // Add o1 and check we have 1 element and can dump.
  {
    rt.Add(o1);
    EXPECT_EQ(1U, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("1 of java.lang.String"), std::string::npos) << oss.str();
    EXPECT_EQ(oss.str().find("short[]"), std::string::npos) << oss.str();
  }

  // Add a second object 10 times and check dumping is sane.
  mirror::Object* o2 = mirror::ShortArray::Alloc(soa.Self(), 0);
  for (size_t i = 0; i < 10; ++i) {
    rt.Add(o2);
    EXPECT_EQ(i + 2, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find(StringPrintf("Last %zd entries (of %zd):",
                                          i + 2 > 10 ? 10 : i + 2,
                                          i + 2)),
              std::string::npos) << oss.str();
    EXPECT_NE(oss.str().find("1 of java.lang.String"), std::string::npos) << oss.str();
    if (i == 0) {
      EXPECT_NE(oss.str().find("1 of short[]"), std::string::npos) << oss.str();
    } else {
      EXPECT_NE(oss.str().find(StringPrintf("%zd of short[] (1 unique instances)", i + 1)),
                std::string::npos) << oss.str();
    }
  }

  // Remove o1 (first element).
  {
    rt.Remove(o1);
    EXPECT_EQ(10U, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_EQ(oss.str().find("java.lang.String"), std::string::npos) << oss.str();
  }

  // Remove o2 ten times.
  for (size_t i = 0; i < 10; ++i) {
    rt.Remove(o2);
    EXPECT_EQ(9 - i, rt.Size());
    std::ostringstream oss;
    rt.Dump(oss);
    if (i == 9) {
      EXPECT_EQ(oss.str().find("short[]"), std::string::npos) << oss.str();
    } else if (i == 8) {
      EXPECT_NE(oss.str().find("1 of short[]"), std::string::npos) << oss.str();
    } else {
      EXPECT_NE(oss.str().find(StringPrintf("%zd of short[] (1 unique instances)", 10 - i - 1)),
                std::string::npos) << oss.str();
    }
  }

  // Add a reference and check that the type of the referent is dumped.
  {
    mirror::Object* empty_reference = CreateWeakReference(nullptr);
    ASSERT_TRUE(empty_reference->IsReferenceInstance());
    rt.Add(empty_reference);
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("java.lang.ref.WeakReference (referent is null)"), std::string::npos)
        << oss.str();
  }

  {
    mirror::Object* string_referent = mirror::String::AllocFromModifiedUtf8(Thread::Current(), "A");
    mirror::Object* non_empty_reference = CreateWeakReference(string_referent);
    ASSERT_TRUE(non_empty_reference->IsReferenceInstance());
    rt.Add(non_empty_reference);
    std::ostringstream oss;
    rt.Dump(oss);
    EXPECT_NE(oss.str().find("java.lang.ref.WeakReference (referent is a java.lang.String)"),
              std::string::npos)
        << oss.str();
  }
}

}  // namespace art
