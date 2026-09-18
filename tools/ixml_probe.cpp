// 探针：复刻 extract_args 的 IXML 遍历，验证 SOAP body 参数提取是否截断
#include <ixml.h>
#include <cstdio>
#include <string>

static const char* kBody =
    "<u:Seek xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
    "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>0:02:58</Target>"
    "</u:Seek>";

int main() {
  IXML_Document* doc = ixmlParseBuffer(kBody);
  if (!doc) { printf("parse FAILED\n"); return 1; }
  IXML_Node* root = ixmlNode_getFirstChild(reinterpret_cast<IXML_Node*>(doc));
  if (!root) { printf("no root\n"); return 1; }
  printf("root name=%s\n", ixmlNode_getNodeName(root) ? ixmlNode_getNodeName(root) : "(null)");
  for (IXML_Node* n = ixmlNode_getFirstChild(root); n; n = ixmlNode_getNextSibling(n)) {
    const char* nm = ixmlNode_getNodeName(n);
    std::string val;
    for (IXML_Node* t = ixmlNode_getFirstChild(n); t; t = ixmlNode_getNextSibling(t)) {
      const char* v = ixmlNode_getNodeValue(t);
      printf("  child node name=%s type=%d value=%s\n",
             ixmlNode_getNodeName(t) ? ixmlNode_getNodeName(t) : "(text)",
             ixmlNode_getNodeType(t), v ? v : "(null)");
      if (v) val += v;
      for (IXML_Node* t2 = ixmlNode_getFirstChild(t); t2; t2 = ixmlNode_getNextSibling(t2)) {
        const char* v2 = ixmlNode_getNodeValue(t2);
        printf("    grandchild type=%d value=%s\n", ixmlNode_getNodeType(t2),
               v2 ? v2 : "(null)");
        if (v2) val += v2;
      }
    }
    printf("ARG %s = %s\n", nm ? nm : "(null)", val.c_str());
  }
  ixmlDocument_free(doc);
  return 0;
}
