#include "HydraAPI.h"
#include "HydraInternal.h"
#include "HydraInternalCommon.h"

#include <memory>
#include <vector>
#include <string>
#include <map>

#include <sstream>
#include <fstream>
#include <iomanip>

#include "HydraObjectManager.h"
#include "xxhash.h"

extern std::wstring      g_lastError;
extern std::wstring      g_lastErrorCallerPlace;
extern HR_ERROR_CALLBACK g_pErrorCallback;
extern HRObjectManager   g_objManager;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static pugi::xml_node my_force_child(pugi::xml_node a_parent, const wchar_t* a_name) ///< helper function
{
  pugi::xml_node child = a_parent.child(a_name);
  if (child != nullptr)
    return child;
  else
    return a_parent.append_child(a_name);
}

static pugi::xml_attribute my_force_attrib(pugi::xml_node a_parent, const wchar_t* a_name) ///< helper function
{
  pugi::xml_attribute attr = a_parent.attribute(a_name);
  if (attr != nullptr)
    return attr;
  else
    return a_parent.append_attribute(a_name);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

HAPI HRTextureNodeRef hrTexture2DCreateFromFile(const wchar_t* a_fileName, int w, int h, int bpp) // no binding HRSceneData and creation of Texture2D
{
  /////////////////////////////////////////////////////////////////////////////////////////////////

  {
    auto p = g_objManager.scnlib().m_textureCache.find(a_fileName);
    if (p != g_objManager.scnlib().m_textureCache.end())
    {
      HRTextureNodeRef ref;
      ref.id = p->second;
      return ref;
    }
  }

  /////////////////////////////////////////////////////////////////////////////////////////////////

  HRTextureNode texRes;
  texRes.name = std::wstring(a_fileName);
  texRes.id   = g_objManager.scnlib().textures.size();
  g_objManager.scnlib().textures.push_back(texRes);

  HRTextureNodeRef ref;
  ref.id = HR_IDType(g_objManager.scnlib().textures.size() - 1);

  HRTextureNode& texture = g_objManager.scnlib().textures[ref.id];
  auto pTextureImpl = g_objManager.m_pFactory->CreateTexture2DFromFile(&texture, a_fileName);
  texture.pImpl = pTextureImpl;
  texture.m_loadedFromFile = true;

  pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

  ChunkPointer chunk(&g_objManager.scnlib().m_vbCache);

  if (pTextureImpl != nullptr)
  {
    auto chunkId = pTextureImpl->chunkId();
    chunk = g_objManager.scnlib().m_vbCache.chunk_at(chunkId);

    w   = pTextureImpl->width();
    h   = pTextureImpl->height();
    bpp = pTextureImpl->bpp();
  }
  else
  {
    HrPrint(HR_SEVERITY_WARNING, L"hrTexture2DCreateFromFile can't open file ", a_fileName);
    g_objManager.scnlib().textures.pop_back();
    HRTextureNodeRef ref2; // dummy white texture
    ref2.id = 0;
    return ref2;
  }

  int byteSize = size_t(w)*size_t(h)*size_t(bpp);

  // form tex name
  //
  const std::wstring id       = ToWString(ref.id);
  const std::wstring location = ChunkName(chunk);
  const std::wstring bytesize = ToWString(byteSize);

	texNodeXml.append_attribute(L"id").set_value(id.c_str());
  texNodeXml.append_attribute(L"name").set_value(a_fileName);
  texNodeXml.append_attribute(L"path").set_value(a_fileName);

  if (pTextureImpl == nullptr)
    texNodeXml.append_attribute(L"loc").set_value(L"unknown");
  else
    g_objManager.SetLoc(texNodeXml, location.c_str());

  texNodeXml.append_attribute(L"offset").set_value(L"8");
  texNodeXml.append_attribute(L"bytesize").set_value(bytesize.c_str());
  texNodeXml.append_attribute(L"width") = w;
  texNodeXml.append_attribute(L"height") = h;
  texNodeXml.append_attribute(L"dl").set_value(L"0");

  g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);
  g_objManager.scnlib().m_textureCache[a_fileName] = ref.id; // remember texture id for given file name

  return ref;
}

HAPI HRTextureNodeRef hrTexture2DCreateFromFileDL(const wchar_t* a_fileName, int w, int h, int bpp)
{
  /////////////////////////////////////////////////////////////////////////////////////////////////
  {
    auto p = g_objManager.scnlib().m_textureCache.find(a_fileName);
    if (p != g_objManager.scnlib().m_textureCache.end())
    {
      HRTextureNodeRef ref;
      ref.id = p->second;
      return ref;
    }
  }
  /////////////////////////////////////////////////////////////////////////////////////////////////

#if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600)
  std::wstring s1(a_fileName);
  std::string  s2(s1.begin(), s1.end());
  std::ifstream testFile(s2.c_str());
#elif defined WIN32
  std::ifstream testFile(a_fileName);
#endif

  if (!testFile.good())
  {
    HrPrint(HR_SEVERITY_WARNING, L"hrTexture2DCreateFromFileDL, bad file ", a_fileName);
    HRTextureNodeRef ref2; // dummy white texture
    ref2.id = 0;
    return ref2;
  }
  else
    testFile.close();

  HRTextureNode texRes;
  texRes.name = std::wstring(a_fileName);
  texRes.id   = g_objManager.scnlib().textures.size();
  g_objManager.scnlib().textures.push_back(texRes);

  HRTextureNodeRef ref;
  ref.id = HR_IDType(g_objManager.scnlib().textures.size() - 1);

  HRTextureNode& texture   = g_objManager.scnlib().textures[ref.id];
  texture.m_loadedFromFile = true;

  pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

  // form tex name
  //
  std::wstring id = ToWString(ref.id);

	texNodeXml.append_attribute(L"id").set_value(id.c_str());
  texNodeXml.append_attribute(L"name").set_value(a_fileName);
  texNodeXml.append_attribute(L"path").set_value(a_fileName);
  //texNodeXml.append_attribute(L"width") = w;  // #TODO: unfortunately we don't know it yet. but we can read headr if possible via vreeimage
  //texNodeXml.append_attribute(L"height") = h; // #TODO: unfortunately we don't know it yet. but we can read headr if possible via vreeimage
  texNodeXml.append_attribute(L"dl").set_value(L"1");

  g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);
  g_objManager.scnlib().m_textureCache[a_fileName] = ref.id; // remember texture id for given file name

  return ref;
}


HAPI HRTextureNodeRef hrTexture2DUpdateFromFile(HRTextureNodeRef currentRef, const wchar_t* a_fileName, int w, int h, int bpp)
{
	g_objManager.scnlib().textures.at(currentRef.id).name = a_fileName;

	HRTextureNodeRef ref;
	ref.id = currentRef.id;

	HRTextureNode& texture = g_objManager.scnlib().textures[ref.id];
	auto pTextureImpl = g_objManager.m_pFactory->CreateTexture2DFromFile(&texture, a_fileName);
	texture.pImpl = pTextureImpl;
	texture.m_loadedFromFile = true;

	pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

	ChunkPointer chunk(&g_objManager.scnlib().m_vbCache);

	if (pTextureImpl != nullptr)
	{
		auto chunkId = pTextureImpl->chunkId();
		chunk = g_objManager.scnlib().m_vbCache.chunk_at(chunkId);

		w = pTextureImpl->width();
		h = pTextureImpl->height();
		bpp = pTextureImpl->bpp();
	}
	else
	{
    HrPrint(HR_SEVERITY_WARNING, L"hrTexture2DUpdateFromFile, can't open file ", a_fileName);
		return currentRef;
	}

	int byteSize = size_t(w)*size_t(h)*size_t(bpp);

	// form tex name
	//
	std::wstring id = ToWString(ref.id);
	std::wstring location = ChunkName(chunk);
	std::wstring bytesize = ToWString(byteSize);

	texNodeXml.append_attribute(L"name").set_value(a_fileName);
	texNodeXml.append_attribute(L"id").set_value(id.c_str());
	texNodeXml.append_attribute(L"path").set_value(a_fileName);

	if (pTextureImpl == nullptr)
		texNodeXml.append_attribute(L"loc").set_value(L"unknown");
	else
    g_objManager.SetLoc(texNodeXml, location.c_str());

	texNodeXml.append_attribute(L"offset").set_value(L"8");
	texNodeXml.append_attribute(L"bytesize").set_value(bytesize.c_str());
  texNodeXml.append_attribute(L"width")  = w;
  texNodeXml.append_attribute(L"height") = h;
  texNodeXml.append_attribute(L"dl").set_value(L"0");

	g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);
	g_objManager.scnlib().m_textureCache[a_fileName] = ref.id; // remember texture id for given file name

	return ref;
}



HAPI HRTextureNodeRef hrTexture2DCreateFromMemory(int w, int h, int bpp, const void* a_data)
{
  if (w == 0 || h == 0 || bpp == 0 || a_data == nullptr)
  {
    HrPrint(HR_SEVERITY_WARNING, L"hrTexture2DCreateFromMemory, invalid input");
    HRTextureNodeRef ref2; // dummy white texture
    ref2.id = 0;
    return ref2;
  }

  /////////////////////////////////////////////////////////////////////////////////////////////////

  std::wstringstream outStr;
  outStr << L"texture2d_" << g_objManager.scnlib().textures.size();

  HRTextureNode texRes; // int w, int h, int bpp, const void* a_data
  texRes.name = outStr.str();
  g_objManager.scnlib().textures.push_back(texRes);

  HRTextureNodeRef ref;
  ref.id = HR_IDType(g_objManager.scnlib().textures.size() - 1);

  HRTextureNode& texture = g_objManager.scnlib().textures[ref.id];
  auto pTextureImpl = g_objManager.m_pFactory->CreateTexture2DFromMemory(&texture, w, h, bpp, a_data);
  texture.pImpl = pTextureImpl;

  pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

  int byteSize = size_t(w)*size_t(h)*size_t(bpp);

  if (pTextureImpl != nullptr)
  {
    ChunkPointer chunk = g_objManager.scnlib().m_vbCache.chunk_at(pTextureImpl->chunkId());

    // form tex name
    //
    std::wstringstream namestr;
    namestr << L"Map#" << ref.id;
    std::wstring texName = namestr.str();
    std::wstring id = ToWString(ref.id);
    std::wstring location = ChunkName(chunk);
    std::wstring bytesize = ToWString(byteSize);

    texNodeXml.append_attribute(L"name").set_value(texName.c_str());
    texNodeXml.append_attribute(L"id").set_value(id.c_str());
    g_objManager.SetLoc(texNodeXml, location.c_str());
    texNodeXml.append_attribute(L"offset").set_value(L"8");
    texNodeXml.append_attribute(L"bytesize").set_value(bytesize.c_str());
    texNodeXml.append_attribute(L"width")  = w;
    texNodeXml.append_attribute(L"height") = h;
    texNodeXml.append_attribute(L"dl").set_value(L"0");

    g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);

    return ref;
  }
  else
  {
    HRTextureNodeRef res;
    res.id = -1;
    return res;
  }
}


HAPI HRTextureNodeRef hrTexture2DUpdateFromMemory(HRTextureNodeRef currentRef, int w, int h, int bpp, const void* a_data)
{
  if (w == 0 || h == 0 || bpp == 0 || a_data == nullptr)
  {
    HrPrint(HR_SEVERITY_WARNING, L"hrTexture2DUpdateFromMemory, invalid input");
    return currentRef;
  }
	/////////////////////////////////////////////////////////////////////////////////////////////////

	std::wstringstream outStr;
	outStr << L"texture2d_" << g_objManager.scnlib().textures.size();

	HRTextureNodeRef ref;
	ref.id = currentRef.id;

	HRTextureNode& texture = g_objManager.scnlib().textures[ref.id];
	auto pTextureImpl = g_objManager.m_pFactory->CreateTexture2DFromMemory(&texture, w, h, bpp, a_data);
	texture.pImpl = pTextureImpl;

	pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

	int byteSize = size_t(w)*size_t(h)*size_t(bpp);

	ChunkPointer chunk = g_objManager.scnlib().m_vbCache.chunk_at(pTextureImpl->chunkId());

	// form tex name
	//
	std::wstringstream namestr;
	namestr << L"Map#" << ref.id;
	std::wstring texName = namestr.str();
	std::wstring id = ToWString(ref.id);
	std::wstring location = ChunkName(chunk);
	std::wstring bytesize = ToWString(byteSize);

	texNodeXml.append_attribute(L"name").set_value(texName.c_str());
	texNodeXml.append_attribute(L"id").set_value(id.c_str());
  g_objManager.SetLoc(texNodeXml, location.c_str());
	texNodeXml.append_attribute(L"offset").set_value(L"8");
	texNodeXml.append_attribute(L"bytesize").set_value(bytesize.c_str());
  texNodeXml.append_attribute(L"width")  = w;
  texNodeXml.append_attribute(L"height") = h;
  texNodeXml.append_attribute(L"dl").set_value(L"0");

	g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);

	return ref;
}

HAPI HRTextureNodeRef hrArray1DCreateFromMemory(const float* a_data, int a_size) // #TODO: implement, add "g_objManager.scnlib().textures[ref.id].Update(texNodeXml);"
{
  if (a_size == 0 || a_data == nullptr)
  {
    HrPrint(HR_SEVERITY_WARNING, L"hrArray1DCreateFromMemory, invalid input");
    HRTextureNodeRef ref2; // dummy white texture
    ref2.id = 0;
    return ref2;
  }

  /////////////////////////////////////////////////////////////////////////////////////////////////
  std::wstringstream outStr;
  outStr << L"array1d_" << g_objManager.scnlib().textures.size();

  HRTextureNode texRes; // const float* a_data, int a_size
  texRes.name = outStr.str();
  g_objManager.scnlib().textures.push_back(texRes);

  HRTextureNodeRef ref;
  ref.id = HR_IDType(g_objManager.scnlib().textures.size() - 1);
  return ref;
}


HAPI void hrTexture2DGetSize(HRTextureNodeRef a_tex, int* pW, int* pH, int* pBPP)
{
  HRTextureNode* pTexture = g_objManager.PtrById(a_tex);

  if (pTexture == nullptr)
  {
    HrError(L"hrTexture2DGetSize: nullptr reference");
    (*pW)   = 0;
    (*pH)   = 0;
    (*pBPP) = 0;
    return;
  }

  auto xml_node = pTexture->xml_node_immediate();

  (*pW)   = xml_node.attribute(L"width").as_int();
  (*pH)   = xml_node.attribute(L"height").as_int();

  const size_t bytesize = (size_t)(xml_node.attribute(L"bytesize").as_ullong());
  (*pBPP) = int(bytesize/size_t((*pW)*(*pH)));
}

HAPI void hrTexture2DGetDataLDR(HRTextureNodeRef a_tex, int* pW, int* pH, int* pData)
{
  HRTextureNode* pTexture = g_objManager.PtrById(a_tex);

  if (pTexture == nullptr)
  {
    (*pW) = 0;
    (*pH) = 0;
    HrError(L"hrTexture2DGetDataLDR: nullptr reference");
    return;
  }

  auto xml_node = pTexture->xml_node_immediate();

  (*pW) = xml_node.attribute(L"width").as_int();
  (*pH) = xml_node.attribute(L"height").as_int();

  if (pTexture->pImpl == nullptr)
  {
    (*pW) = 0;
    (*pH) = 0;
    HrError(L"hrTexture2DGetDataLDR: nullptr texture data");
    return;
  }

  auto chunkId  = pTexture->pImpl->chunkId();
  auto chunk    = g_objManager.scnlib().m_vbCache.chunk_at(chunkId);
  auto bytesize = xml_node.attribute(L"bytesize").as_int();
  auto offset   = xml_node.attribute(L"offset").as_int();

  char* data = (char*)chunk.GetMemoryNow();
  if (data == nullptr)
  {
#ifdef WIN32
    const std::wstring loc = g_objManager.GetLoc(xml_node);   // load from file from "loc" #TODO: find a way to test it in proper way.
#else
    std::wstring s1(g_objManager.GetLoc(xml_node));
    const std::string  loc(s1.begin(), s1.end());
#endif
    std::ifstream fin(loc.c_str(), std::ios::binary);
    fin.seekg(offset);
    fin.read((char*)pData, bytesize);
    fin.close();
  }
  else
    memcpy(pData, data + offset, bytesize);
}



HAPI void hrTextureNodeOpen(HRTextureNodeRef a_pNode)
{
  HRTextureNode* pData = g_objManager.PtrById(a_pNode);

  if (pData == nullptr)
  {
    HrError(L"hrTextureNodeOpen: nullptr reference");
    return;
  }

  pData->opened = true;

}

HAPI void hrTextureNodeClose(HRTextureNodeRef a_pNode)
{
  HRTextureNode* pData = g_objManager.PtrById(a_pNode);

  if (pData == nullptr)
  {
    HrError(L"hrTextureNodeClose: nullptr reference");
    return;
  }

  pData->opened = false;
  pData->pImpl = nullptr;
}



HAPI pugi::xml_node hrTextureBind(HRTextureNodeRef a_pTexNode, pugi::xml_node a_node)
{
  HRTextureNode* pData = g_objManager.PtrById(a_pTexNode);
  if (pData == nullptr)
  {
    pugi::xml_node texNode = a_node.child(L"texture"); // delet texture
    texNode.parent().remove_child(texNode);
    return pugi::xml_node();
  }

  // add a_pTexNode to special list of material ... ? -> can do this later when close function works !!!
	//
  pugi::xml_node texNode = a_node.child(L"texture");
  if (texNode == nullptr)
    texNode = a_node.append_child(L"texture");

  my_force_attrib(texNode, L"id").set_value(a_pTexNode.id);
  my_force_attrib(texNode, L"type").set_value(L"texref");

	return texNode;
}


HAPI HRTextureNodeRef  hrTexture2DCreateFromProcLDR(HR_TEXTURE2D_PROC_LDR_CALLBACK a_proc, void* a_customData, int w, int h)
{
	int bpp = 4;

	//#TODO : determine resolution
  if ((w == -1) && (h == -1))
  {
    w = 32;
    h = 32;
  }

	unsigned char* imageData = new unsigned char[w*h*bpp];

	a_proc(imageData, w, h, a_customData);

	HRTextureNodeRef procTex = hrTexture2DCreateFromMemory(w, h, 4, imageData);

	delete [] imageData;
	
	return procTex;
}

HAPI HRTextureNodeRef  hrTexture2DCreateFromProcHDR(HR_TEXTURE2D_PROC_HDR_CALLBACK a_proc, void* a_customData, int w, int h)
{
	int bpp = sizeof(float)*4;

	//TODO : determine resolution
  if ((w == -1) && (h == -1))
  {
    w = 32;
    h = 32;
  }

	float* imageData = new float[w*h*bpp/sizeof(float)];

	a_proc(imageData, w, h, a_customData);

	HRTextureNodeRef procTex = hrTexture2DCreateFromMemory(w, h, bpp, imageData);

	delete [] imageData;

	return procTex;
}

HAPI HRTextureNodeRef  hrTexture2DUpdateFromProcLDR(HRTextureNodeRef currentRef, HR_TEXTURE2D_PROC_LDR_CALLBACK a_proc, void* a_customData, int w, int h)
{
  int bpp = 4;

  //#TODO : determine resolution
  if ((w == -1) && (h == -1))
  {
    w = 32;
    h = 32;
  }

  unsigned char* imageData = new unsigned char[w*h*bpp];

  a_proc(imageData, w, h, a_customData);

  HRTextureNodeRef procTex = hrTexture2DUpdateFromMemory(currentRef, w, h, 4, imageData);

  delete[] imageData;

  return procTex;
}

HAPI HRTextureNodeRef  hrTexture2DUpdateFromProcHDR(HRTextureNodeRef currentRef, HR_TEXTURE2D_PROC_HDR_CALLBACK a_proc, void* a_customData, int w, int h)
{
  int bpp = sizeof(float) * 4;

  //TODO : determine resolution
  if ((w == -1) && (h == -1))
  {
    w = 32;
    h = 32;
  }

  float* imageData = new float[w*h*bpp / sizeof(float)];

  a_proc(imageData, w, h, a_customData);

  HRTextureNodeRef procTex = hrTexture2DUpdateFromMemory(currentRef, w, h, bpp, imageData);

  delete[] imageData;

  return procTex;
}


HAPI HRTextureNodeRef hrTextureCreateAdvanced(const wchar_t* a_texType, const wchar_t* a_objName)
{
	/////////////////////////////////////////////////////////////////////////////////////////////////
	//TODO : write some real implementation


	HRTextureNode texRes;
	texRes.name = std::wstring(a_objName);
	texRes.id = g_objManager.scnlib().textures.size();
	g_objManager.scnlib().textures.push_back(texRes);

	HRTextureNodeRef ref;
	ref.id = HR_IDType(g_objManager.scnlib().textures.size() - 1);

	HRTextureNode& texture = g_objManager.scnlib().textures[ref.id];
	auto pTextureImpl = nullptr;
	texture.pImpl = pTextureImpl;

	pugi::xml_node texNodeXml = g_objManager.textures_lib_append_child();

	std::wstring id = ToWString(ref.id);
	std::wstring type = a_texType;


	texNodeXml.append_attribute(L"id").set_value(id.c_str());
	texNodeXml.append_attribute(L"name").set_value(a_objName);
	texNodeXml.append_attribute(L"type").set_value(type.c_str());

	g_objManager.scnlib().textures[ref.id].update_next(texNodeXml);

	return ref;
}

HAPI pugi::xml_node hrTextureParamNode(HRTextureNodeRef a_texRef)
{
	HRTextureNode* pTex = g_objManager.PtrById(a_texRef);
	if (pTex == nullptr)
	{
		HrError(L"hrTextureParamNode, nullptr input ");
		return pugi::xml_node();
	}

	if (!pTex->opened)
	{
    HrError(L"hrTextureParamNode, texture is not opened, texture id = ", pTex->id);
		return  pugi::xml_node();
	}

	return pTex->xml_node_next(pTex->openMode);
}
