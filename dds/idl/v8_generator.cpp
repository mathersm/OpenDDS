/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "v8_generator.h"
#include "be_extern.h"

#include "utl_identifier.h"

using namespace AstTypeClassification;

void v8_generator::gen_includes()
{
  be_global->add_include("<v8.h>", BE_GlobalData::STREAM_H);
  be_global->add_include("<nan.h>", BE_GlobalData::STREAM_CPP);
}

bool v8_generator::gen_enum(AST_Enum*, UTL_ScopedName*,
                            const std::vector<AST_EnumVal*>&, const char*)
{
  return true;
}

namespace {

  std::string getV8Type(AST_Type* type,
                        AST_PredefinedType::PredefinedType* pt = 0)
  {
    const Classification cls = classify(type);
    if (cls & (CL_ENUM | CL_STRING)) return "String";
    if (cls & (CL_STRUCTURE | CL_UNION)) return "Object";
    if (cls & (CL_ARRAY | CL_SEQUENCE)) return "Array";
    if (cls & CL_PRIMITIVE) {
      AST_Type* actual = resolveActualType(type);
      AST_PredefinedType* p = AST_PredefinedType::narrow_from_decl(actual);
      if (pt) *pt = p->pt();
      switch (p->pt()) {
      case AST_PredefinedType::PT_char:
      case AST_PredefinedType::PT_wchar:
        return "String";
      case AST_PredefinedType::PT_boolean:
        return "Boolean";
      case AST_PredefinedType::PT_octet:
      case AST_PredefinedType::PT_ushort:
      case AST_PredefinedType::PT_short:
      case AST_PredefinedType::PT_ulong:
      case AST_PredefinedType::PT_long:
        return "Integer";
      default:
        return "Number";
      }
    }
    return "";
  }

  bool builtInSeq(AST_Type* type)
  {
    const std::string name = scoped(type->name());
    static const char PRE[] = "CORBA::";
    if (std::strncmp(name.c_str(), PRE, sizeof(PRE) - 1)) return false;
    return name.rfind("Seq") == name.size() - 3;
  }

  void gen_copyto(const char* tgt, const char* src, AST_Type* type,
                  const char* prop, std::ostream& strm = be_global->impl_)
  {
    const Classification cls = classify(type);
    AST_PredefinedType::PredefinedType pt = AST_PredefinedType::PT_void;
    const std::string v8Type = getV8Type(type, &pt),
      propName = std::string(tgt) + "->Set(" + prop + ", ";

    if (cls & CL_SCALAR) {
      std::string prefix, suffix, postNew;

      if (cls & CL_ENUM) {
        const std::string underscores =
          dds_generator::scoped_helper(type->name(), "_"),
          array = "gen_" + underscores + "_names";
        prefix = '(' + std::string(src) + " >= sizeof(" + array + ")/sizeof(" +
          array + "[0])) ? \"<<invalid>>\" : " + array + "[static_cast<int>(";
        suffix = ")]";
        postNew = ".ToLocalChecked()";

      } else if (cls & CL_STRING) {
        if (!std::strstr(src, "()")) {
          suffix = ".in()";
        }
        if (cls & CL_WIDE) {
          strm <<
            "  {\n"
            "    const size_t len = ACE_OS::strlen(" << src << suffix << ");\n"
            "    uint16_t* const str = new uint16_t[len + 1];\n"
            "    for (size_t i = 0; i <= len; ++i) {\n"
            "      str[i] = " << src << "[i];\n"
            "    }\n"
            "    " << propName << "Nan::New(str).ToLocalChecked());\n"
            "    delete[] str;\n"
            "  }\n";
          return;
        }
        postNew = ".ToLocalChecked()";

      } else if (pt == AST_PredefinedType::PT_char) {
        strm <<
          "  {\n"
          "    const char str[] = {" << src << ", 0};\n"
          "    " << propName << "Nan::New(str).ToLocalChecked());\n"
          "  }\n";
        return;

      } else if (pt == AST_PredefinedType::PT_wchar) {
        strm <<
          "  {\n"
          "    const uint16_t str[] = {" << src << ", 0};\n"
          "    " << propName << "Nan::New(str).ToLocalChecked());\n"
          "  }\n";
        return;

      } else if (v8Type == "Number") {
        prefix = "static_cast<double>(";
        suffix = ")";
      }

      strm <<
        "  " << propName << "Nan::New(" << prefix << src << suffix << ")"
             << postNew << ");\n";

    } else if ((cls & CL_SEQUENCE) && builtInSeq(type)) {
      AST_Type* real_type = resolveActualType(type);
      AST_Sequence* seq = AST_Sequence::narrow_from_decl(real_type);
      AST_Type* elem = seq->base_type();
      strm <<
        "  {\n"
        "    const v8::Local<v8::Array> seq = Nan::New<v8::Array>(" << src
                       << ".length());\n"
        "    for (CORBA::ULong j = 0; j < " << src << ".length(); ++j) {\n";
      gen_copyto("seq", (std::string(src) + "[j]").c_str(), elem, "j");
      strm <<
        "    }\n"
        "    " << propName << "seq" << ");\n"
        "  }\n";

    } else { // struct, sequence, etc.
      strm <<
        "  " << propName << "copyToV8(" << src << "));\n";
    }
  }

  struct gen_field_copyto {
    gen_field_copyto(const char* tgt, const char* src) : tgt_(tgt), src_(src) {}
    const std::string tgt_, src_;
    void operator()(AST_Field* field) const
    {
      const std::string fieldName = field->local_name()->get_string(),
        source = src_ + '.' + fieldName,
        prop = "Nan::New<v8::String>(\"" + fieldName + "\").ToLocalChecked()";
      gen_copyto(tgt_.c_str(), source.c_str(),
                 field->field_type(), prop.c_str());
    }
  };
}

bool v8_generator::gen_struct(AST_Structure*, UTL_ScopedName* name,
                              const std::vector<AST_Field*>& fields,
                              AST_Type::SIZE_TYPE, const char*)
{
  gen_includes();
  {
    NamespaceGuard ng;
    const std::string clazz = scoped(name);
    {
      Function ctv("copyToV8", "v8::Local<v8::Object>");
      ctv.addArg("src", "const " + clazz + '&');
      ctv.endArgs();
      be_global->impl_ <<
        "  const v8::Local<v8::Object> stru = Nan::New<v8::Object>();\n";
      std::for_each(fields.begin(), fields.end(),
                    gen_field_copyto("stru", "src"));
      be_global->impl_ <<
        "  return stru;\n";
    }
  }

  if (idl_global->is_dcps_type(name)) {
    be_global->add_include("dds/DCPS/V8TypeConverter.h",
                           BE_GlobalData::STREAM_CPP);
    ScopedNamespaceGuard cppGuard(name, be_global->impl_);
    const std::string lname = name->last_component()->get_string(),
      tsv8 = lname + "TypeSupportV8Impl";
    be_global->impl_ <<
      "class " << tsv8 << "\n"
      "  : public virtual " << lname << "TypeSupportImpl\n"
      "  , public virtual OpenDDS::DCPS::V8TypeConverter {\n\n"
      "  v8::Local<v8::Object> toV8(const void* source) const\n"
      "  {\n"
      "    return OpenDDS::DCPS::copyToV8(*static_cast<const " << lname
               << "*>(source));\n"
      "  }\n\n"
      "public:\n"
      "  struct Initializer {\n"
      "    Initializer()\n"
      "    {\n"
      "      " << lname << "TypeSupport_var ts = new " << tsv8 << ";\n"
      "      ts->register_type(0, \"\");\n"
      "    }\n"
      "  };\n"
      "};\n\n" <<
      tsv8 << "::Initializer init_tsv8_" << lname << ";\n";
  }
  return true;
}

bool v8_generator::gen_typedef(AST_Typedef*, UTL_ScopedName* name,
                               AST_Type* type, const char* /*repoid*/)
{
  gen_includes();
  switch (type->node_type()) {
  case AST_Decl::NT_sequence: {
    NamespaceGuard ng;
    AST_Sequence* seq = AST_Sequence::narrow_from_decl(type);
    const std::string cxx = scoped(name);
    AST_Type* elem = seq->base_type();
    {
      Function ctv("copyToV8", "v8::Local<v8::Object>");
      ctv.addArg("src", "const " + cxx + '&');
      ctv.endArgs();
      be_global->impl_ <<
        "  const v8::Local<v8::Array> tgt(Nan::New<v8::Array>(src.length()));\n"
        "  for (CORBA::ULong i = 0; i < src.length(); ++i) {\n"
        "  ";
      gen_copyto("tgt", "src[i]", elem, "i");
      be_global->impl_ <<
        "  }\n"
        "  return tgt;\n";
    }
    break;
  }
  case AST_Decl::NT_array: {
    //TODO: support arrays
    break;
  }
  default:
    return true;
  }
  return true;
}

namespace {
  std::string branchGen(const std::string& name, AST_Type* type,
                        const std::string&, std::string&,
                        const std::string&)
  {
    const std::string source = "src." + name + "()",
      prop = "Nan::New<v8::String>(\"" + name + "\").ToLocalChecked()";
    std::ostringstream strm;
    strm << "  ";
    gen_copyto("uni", source.c_str(), type, prop.c_str(), strm);
    return strm.str();
  }
}

bool v8_generator::gen_union(AST_Union*, UTL_ScopedName* name,
                             const std::vector<AST_UnionBranch*>& branches,
                             AST_Type* discriminator, const char* /*repoid*/)
{
  gen_includes();
  NamespaceGuard ng;
  const std::string clazz = scoped(name);
  {
    Function ctv("copyToV8", "v8::Local<v8::Object>");
    ctv.addArg("src", "const " + clazz + '&');
    ctv.endArgs();
    be_global->impl_ <<
      "  const v8::Local<v8::Object> uni = Nan::New<v8::Object>();\n";
    gen_copyto("uni", "src._d()", discriminator, "Nan::New<v8::String>(\"_d\").ToLocalChecked()");
    generateSwitchForUnion("src._d()", branchGen, branches, discriminator,
                           "", "", clazz.c_str(), false, false);
    be_global->impl_ <<
      "  return uni;\n";
  }
  return true;
}
