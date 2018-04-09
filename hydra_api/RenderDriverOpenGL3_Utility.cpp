//
// Created by vsan on 23.03.18.
//

#include <cmath>
#include "RenderDriverOpenGL3_Utility.h"
#include "HydraObjectManager.h"


RD_OGL32_Utility::RD_OGL32_Utility()
{
  camFov       = 45.0f;
  camNearPlane = 0.05f;
  camFarPlane  = 10000.0f;

  camPos[0] = 0.0f; camPos[1] = 0.0f; camPos[2] = 0.0f;
  camLookAt[0] = 0.0f; camLookAt[1] = 0.0f; camLookAt[2] = -1.0f;

  m_width  = 1024;
  m_height = 1024;

  m_texNum = 0;

  m_quad = std::make_unique<FullScreenQuad>();
  //m_fullScreenTexture = std::make_unique<RenderTexture2D>(GL_RGBA, GL_RGBA32F, m_width, m_height);
  m_lodBuffer = std::make_unique<LODBuffer>(m_width, m_height);

  m_matricesUBOBindingPoint = 0;
}


void RD_OGL32_Utility::ClearAll()
{
  m_lodBufferProgram.Release();
  m_quadProgram.Release();

  m_objects.clear();

  m_materials_pt1.clear();
  m_materials_pt2.clear();
}

HRDriverAllocInfo RD_OGL32_Utility::AllocAll(HRDriverAllocInfo a_info)
{
  //m_objects.resize(a_info.geomNum);

  m_libPath = std::wstring(a_info.libraryPath);

  std::unordered_map<GLenum, std::string> lodBufferShaders;
  lodBufferShaders[GL_VERTEX_SHADER] = "../glsl/LODBuffer.vert";
  lodBufferShaders[GL_FRAGMENT_SHADER] = "../glsl/LODBuffer.frag";
  m_lodBufferProgram = ShaderProgram(lodBufferShaders);

  std::unordered_map<GLenum, std::string> quadShaders;
  quadShaders[GL_VERTEX_SHADER] = "../glsl/vQuad.vert";
  quadShaders[GL_FRAGMENT_SHADER] = "../glsl/fQuadDebug.frag";
  m_quadProgram = ShaderProgram(quadShaders);

  m_texNum = (unsigned int)a_info.imgNum;

  m_materials_pt1.resize((unsigned long)(a_info.matNum), int4(-1, -1, -1, -1));
  m_materials_pt2.resize((unsigned long)(a_info.matNum), int4(-1, -1, -1, -1));


  glGenTextures(1, &m_whiteTex);
  CreatePlaceholderWhiteTexture(m_whiteTex);

  CreateMaterialsTBO();
  CreateMatricesUBO();

  return a_info;
}

HRDriverInfo RD_OGL32_Utility::Info()
{
  HRDriverInfo info;

  info.supportHDRFrameBuffer        = false;
  info.supportHDRTextures           = true;
  info.supportMultiMaterialInstance = false;

  info.supportImageLoadFromInternalFormat = false;
  info.supportImageLoadFromExternalFormat = false;
  info.supportMeshLoadFromInternalFormat  = false;
  info.supportLighting                    = false;

  info.memTotal = int64_t(8) * int64_t(1024 * 1024 * 1024); //TODO: ?

  return info;
}

bool RD_OGL32_Utility::UpdateImage(int32_t a_texId, int32_t w, int32_t h, int32_t bpp, const void *a_data,
                                    pugi::xml_node a_texNode)
{
  return true;
}


bool RD_OGL32_Utility::UpdateMaterial(int32_t a_matId, pugi::xml_node a_materialNode)
{
  auto mat_id = (unsigned int)(a_matId);

  int32_t emissionTexId = -1;
  int32_t diffuseTexId = -1;
  int32_t reflectTexId = -1;
  int32_t reflectGlossTexId = -1;

  int32_t transparencyTexId = -1;
  int32_t opacityTexId = -1;
  int32_t translucencyTexId = -1;
  int32_t bumpTexId = -1;

  auto emissionTex = a_materialNode.child(L"emission").child(L"color").child(L"texture");
  if (emissionTex  != nullptr)
    emissionTexId = emissionTex.attribute(L"id").as_int();

  auto diffuseTex = a_materialNode.child(L"diffuse").child(L"color").child(L"texture");
  if (diffuseTex  != nullptr)
    diffuseTexId = diffuseTex.attribute(L"id").as_int();

  auto reflectTex = a_materialNode.child(L"reflectivity").child(L"color").child(L"texture");
  if (reflectTex  != nullptr)
    reflectTexId = reflectTex.attribute(L"id").as_int();

  
  auto reflectGlossTex = a_materialNode.child(L"reflectivity").child(L"glossiness").child(L"texture");
  if (reflectGlossTex  != nullptr)
    reflectGlossTexId = reflectGlossTex.attribute(L"id").as_int();

  auto transparencyTex = a_materialNode.child(L"transparency").child(L"color").child(L"texture");
  if (transparencyTex  != nullptr)
    transparencyTexId = transparencyTex.attribute(L"id").as_int();
  
  auto opacityTex = a_materialNode.child(L"opacity").child(L"texture");
  if (opacityTex  != nullptr)
    opacityTexId = opacityTex.attribute(L"id").as_int();

  auto translucencyTex = a_materialNode.child(L"translucency").child(L"color").child(L"texture");
  if (translucencyTex  != nullptr)
    translucencyTexId = translucencyTex.attribute(L"id").as_int();

  auto bumpTex = a_materialNode.child(L"displacement").child(L"normal_map").child(L"texture");
  if (bumpTex  != nullptr)
    bumpTexId = bumpTex.attribute(L"id").as_int();


  int4 mat_pt1 = int4(emissionTexId, diffuseTexId, reflectTexId, reflectGlossTexId);
  int4 mat_pt2 = int4(transparencyTexId, opacityTexId, translucencyTexId, bumpTexId);

//  int4 mat_pt1 = int4(3, 4, 5, 6);
//  int4 mat_pt2 = int4(7, 8, 9, 10);

  m_materials_pt1.at(mat_id) = mat_pt1;
  m_materials_pt2.at(mat_id) = mat_pt2;


  return true;
}

bool RD_OGL32_Utility::UpdateLight(int32_t a_lightId, pugi::xml_node a_lightNode)
{
  return true;
}

bool RD_OGL32_Utility::UpdateMesh(int32_t a_meshId, pugi::xml_node a_meshNode, const HRMeshDriverInput &a_input,
                                   const HRBatchInfo *a_batchList, int32_t a_listSize)
{
  if (a_input.triNum == 0)
  {
    return true;
  }
  //TODO: maybe try MultiDrawIndirect

  meshData batchMeshData;
  for (int32_t batchId = 0; batchId < a_listSize; ++batchId)
  {
    HRBatchInfo batch = a_batchList[batchId];

    GLuint vertexPosBufferObject;
    GLuint vertexNormBufferObject;
    GLuint vertexTangentBufferObject;
    GLuint vertexTexCoordsBufferObject;
    //   GLuint matIDBufferObject;

    GLuint indexBufferObject;
    GLuint vertexArrayObject;

    std::vector<float> batchPos;
    std::vector<float> batchNorm;
    std::vector<float> batchTangent;
    std::vector<float> batchTexCoord;
    std::vector<int>   batchIndices;

    CreateGeometryFromBatch(batch, a_input, batchPos, batchNorm, batchTangent, batchTexCoord, batchIndices);

    //   std::vector<int>   matIDs(batchPos.size() / 4, batch.matId);

    // vertex positions
    glGenBuffers(1, &vertexPosBufferObject);
    glBindBuffer(GL_ARRAY_BUFFER, vertexPosBufferObject);
    glBufferData(GL_ARRAY_BUFFER, batchPos.size() * sizeof(GLfloat), &batchPos[0], GL_STATIC_DRAW);

    // vertex normals
    glGenBuffers(1, &vertexNormBufferObject);
    glBindBuffer(GL_ARRAY_BUFFER, vertexNormBufferObject);
    glBufferData(GL_ARRAY_BUFFER, batchNorm.size() * sizeof(GLfloat), &batchNorm[0], GL_STATIC_DRAW);

    // vertex texture coordinates
    glGenBuffers(1, &vertexTexCoordsBufferObject);
    glBindBuffer(GL_ARRAY_BUFFER, vertexTexCoordsBufferObject);
    glBufferData(GL_ARRAY_BUFFER, batchTexCoord.size() * sizeof(GLfloat), &batchTexCoord[0], GL_STATIC_DRAW);

    //matID
/*    glGenBuffers(1, &matIDBufferObject);
    glBindBuffer(GL_ARRAY_BUFFER, matIDBufferObject);
    glBufferData(GL_ARRAY_BUFFER, matIDs.size() * sizeof(GLint), &matIDs[0], GL_STATIC_DRAW);
*/
    // index buffer
    glGenBuffers(1, &indexBufferObject);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBufferObject);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, batchIndices.size() * sizeof(GLuint), &batchIndices[0], GL_STATIC_DRAW);


    glGenVertexArrays(1, &vertexArrayObject);
    glBindVertexArray(vertexArrayObject);

    glBindBuffer(GL_ARRAY_BUFFER, vertexPosBufferObject);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, vertexNormBufferObject);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, vertexTexCoordsBufferObject);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    /*   glBindBuffer(GL_ARRAY_BUFFER, matIDBufferObject);
       glEnableVertexAttribArray(4);
       glVertexAttribPointer(4, 1, GL_INT, GL_FALSE, 0, nullptr);
   
       */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBufferObject);

    glBindVertexArray(0);

    std::pair<GLuint, int> tmp;
    tmp.first = vertexArrayObject;
    tmp.second = int(batchIndices.size());

    batchMeshData[batch.matId] = tmp;
  }

  m_objects[a_meshId] = batchMeshData;

  return true;
}


bool RD_OGL32_Utility::UpdateCamera(pugi::xml_node a_camNode)
{
  if (a_camNode == nullptr)
    return true;

  const std::wstring camPosStr = a_camNode.child(L"position").text().as_string();
  const std::wstring camLAtStr = a_camNode.child(L"look_at").text().as_string();
  const std::wstring camUpStr  = a_camNode.child(L"up").text().as_string();

  if (!a_camNode.child(L"fov").text().empty())
    camFov = a_camNode.child(L"fov").text().as_float();

  if (!a_camNode.child(L"nearClipPlane").text().empty())
    camNearPlane = 0.1f;// a_camNode.child(L"nearClipPlane").text().as_float();

  if (!a_camNode.child(L"farClipPlane").text().empty())
    camFarPlane = 1000000.0f;//a_camNode.child(L"farClipPlane").text().as_float();

  if (!camPosStr.empty())
  {
    std::wstringstream input(camPosStr);
    input >> camPos[0] >> camPos[1] >> camPos[2];
  }

  if (!camLAtStr.empty())
  {
    std::wstringstream input(camLAtStr);
    input >> camLookAt[0] >> camLookAt[1] >> camLookAt[2];
  }

  if (!camUpStr.empty())
  {
    std::wstringstream input(camUpStr);
    input >> camUp[0] >> camUp[1] >> camUp[2];
  }

  return true;
}

bool RD_OGL32_Utility::UpdateSettings(pugi::xml_node a_settingsNode)
{
  if (a_settingsNode.child(L"width") != nullptr)
    m_settingsWidth = min(a_settingsNode.child(L"width").text().as_int(), MAX_TEXTURE_RESOLUTION);

  if (a_settingsNode.child(L"height") != nullptr)
    m_settingsHeight = min(a_settingsNode.child(L"height").text().as_int(), MAX_TEXTURE_RESOLUTION);

  return true;
}

void RD_OGL32_Utility::BeginScene(pugi::xml_node a_sceneNode)
{
  // std::cout << "BeginScene" <<std::endl;
  glViewport(0, 0, (GLint)m_width, (GLint)m_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  m_lodBufferProgram.StartUseShader();
  m_lodBuffer->StartRendering();

  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_DEPTH_CLAMP);
  glClear(GL_DEPTH_BUFFER_BIT);

  const float aspect = float(m_width) / float(m_height);
  projection = projectionMatrixTransposed(camFov, aspect, camNearPlane, camFarPlane);

  float3 eye(camPos[0], camPos[1], camPos[2]);
  float3 center(camLookAt[0], camLookAt[1], camLookAt[2]);
  float3 up(camUp[0], camUp[1], camUp[2]);
  lookAt = lookAtTransposed(eye, center, up);

  std::vector<float4x4> matrices{lookAt , projection };

  glBindBuffer(GL_UNIFORM_BUFFER, m_matricesUBO);
  //glBufferData(GL_UNIFORM_BUFFER, 32 * sizeof(GLfloat), &matrices[0], GL_STATIC_DRAW);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, 32 * sizeof(GLfloat), &matrices[0]);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);


  SetMaterialsTBO();

  //m_lodBufferProgram.SetUniform("window_res", int2(m_width, m_height));
  m_lodBufferProgram.SetUniform("window_res", int2(m_settingsWidth, m_settingsHeight));
}



void RD_OGL32_Utility::EndScene()
{
  m_lodBuffer->EndRendering();

  FillMipLevelsDict();

  m_quadProgram.StartUseShader();
  bindTexture(m_quadProgram, 0, "debugTex", m_lodBuffer->GetTextureId(LODBuffer::LODBUF_TEX_1));

  m_quad->Draw();

  //m_quadProgram.StopUseShader();

  /* Direct copy of render texture to the default framebuffer */
  /*
  m_gBuffer->StartFinalPass(0);
  glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  */

  /*for (std::pair<int32_t, int32_t> elem : dict)
    std::cout << " " << elem.first << ":" << elem.second << std::endl;*/

  glFlush();
}


void RD_OGL32_Utility::InstanceMeshes(int32_t a_mesh_id, const float *a_matrices, int32_t a_instNum,
                                       const int *a_lightInstId, const int* a_remapId)
{
  // std::cout << "InstanceMeshes" <<std::endl;

  for (int32_t i = 0; i < a_instNum; i++)
  {
    float modelM[16];
    mat4x4_transpose(modelM, (float*)(a_matrices + i*16));

    m_lodBufferProgram.SetUniform("model", float4x4(modelM));

    for(auto batch : m_objects[a_mesh_id])
    {
      int matId = batch.first;

      m_lodBufferProgram.SetUniform("matID", matId);

      bindTextureBuffer(m_lodBufferProgram, 0, "materials1", m_materialsTBOs[0], m_materialsTBOTexIds[0], GL_RGBA32I);
      bindTextureBuffer(m_lodBufferProgram, 1, "materials2", m_materialsTBOs[1], m_materialsTBOTexIds[1], GL_RGBA32I);

      glBindVertexArray(batch.second.first);
      glDrawElements(GL_TRIANGLES, batch.second.second, GL_UNSIGNED_INT, nullptr);
      glBindVertexArray(0);

      m_lodBufferProgram.SetUniform("matID", -1);
    }

  }
}


void RD_OGL32_Utility::InstanceLights(int32_t a_light_id, const float *a_matrix, pugi::xml_node* a_custAttrArray, int32_t a_instNum, int32_t a_lightGroupId)
{

}

void RD_OGL32_Utility::CreateMaterialsTBO()
{
  glGenBuffers(2, m_materialsTBOs);

  glBindBuffer(GL_TEXTURE_BUFFER, m_materialsTBOs[0]);
  glBufferData(GL_TEXTURE_BUFFER, sizeof(int32_t) * 4 * m_materials_pt1.size(), nullptr, GL_STATIC_DRAW);
  glGenTextures(1, &m_materialsTBOTexIds[0]);

  glBindBuffer(GL_TEXTURE_BUFFER, m_materialsTBOs[1]);
  glBufferData(GL_TEXTURE_BUFFER, sizeof(int32_t) * 4 * m_materials_pt2.size(), nullptr, GL_STATIC_DRAW);
  glGenTextures(1, &m_materialsTBOTexIds[1]);

  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void RD_OGL32_Utility::SetMaterialsTBO()
{
  glBindBuffer(GL_TEXTURE_BUFFER, m_materialsTBOs[0]);
  glBufferSubData(GL_TEXTURE_BUFFER, 0,  sizeof(int) * 4 * m_materials_pt1.size(), &m_materials_pt1[0]);

  glBindBuffer(GL_TEXTURE_BUFFER, m_materialsTBOs[1]);
  glBufferSubData(GL_TEXTURE_BUFFER, 0,  sizeof(int) * 4 * m_materials_pt2.size(), &m_materials_pt2[0]);

  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void RD_OGL32_Utility::CreateMatricesUBO()
{
  GLuint uboIndexLODBuffer   = glGetUniformBlockIndex(m_lodBufferProgram.GetProgram(), "matrixBuffer");

  glUniformBlockBinding(m_lodBufferProgram.GetProgram(),    uboIndexLODBuffer,   m_matricesUBOBindingPoint);

  GLsizei matricesUBOSize = 32 * sizeof(GLfloat);

  glGenBuffers(1, &m_matricesUBO);

  glBindBuffer(GL_UNIFORM_BUFFER, m_matricesUBO);
  glBufferData(GL_UNIFORM_BUFFER, matricesUBOSize, nullptr, GL_STATIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  glBindBufferRange(GL_UNIFORM_BUFFER, m_matricesUBOBindingPoint, m_matricesUBO, 0, matricesUBOSize);

}

void RD_OGL32_Utility::Draw()
{
  //std::cout << "Draw" << std::endl;

}

void RD_OGL32_Utility::FillMipLevelsDict()
{
  std::vector<unsigned int> texture_data((unsigned long)(m_width * m_height * 4 * 2), 0);

  glBindTexture(GL_TEXTURE_2D, m_lodBuffer->GetTextureId(LODBuffer::LODBUF_TEX_1));
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, &texture_data[0]);

  glBindTexture(GL_TEXTURE_2D, m_lodBuffer->GetTextureId(LODBuffer::LODBUF_TEX_2));
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, &texture_data[m_width * m_height * 4]);

  const unsigned int texIdBits    = 0x00FFFFFFu;
  const unsigned int mipLevelBits = 0xFF000000u;

  m_mipLevelDict.clear();


  for(auto pix : texture_data)
  {
    uint32_t mipLevel = pix >> 24u;
    uint32_t texId    = pix & texIdBits;

    if(texId <= m_texNum)
    {
      if (m_mipLevelDict.find(texId) != m_mipLevelDict.end())
      {
        if (m_mipLevelDict[texId] > mipLevel)
          m_mipLevelDict[texId] = mipLevel;
      } else
        m_mipLevelDict[texId] = mipLevel;
    }
  }
}

HRRenderUpdateInfo RD_OGL32_Utility::HaveUpdateNow(int a_maxRaysPerPixel)
{
  HRRenderUpdateInfo res;
  res.finalUpdate   = true;
  res.haveUpdateFB  = true;
  res.progress      = 100.0f;
  return res;
}

void RD_OGL32_Utility::GetFrameBufferHDR(int32_t w, int32_t h, float *a_out, const wchar_t *a_layerName)
{
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, (GLvoid*)a_out);
}

void RD_OGL32_Utility::GetFrameBufferLDR(int32_t w, int32_t h, int32_t *a_out)
{
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)a_out);
}


IHRRenderDriver* CreateOpenGL3_Utilty_RenderDriver()
{
  return new RD_OGL32_Utility;
}


GLFWwindow * InitGLForUtilityDriver()
{
  GLFWwindow *offscreen_context = nullptr;

  //bool init_result = _init_GL_for_utility_driver(offscreen_context);

  if (!glfwInit())
  {
    HrError(L"Failed to initialize GLFW for Utility driver");
    return nullptr;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  offscreen_context = glfwCreateWindow(1024, 1024, "", NULL, NULL);
  glfwMakeContextCurrent(offscreen_context);

  if (!offscreen_context)
  {
    HrError(L"Failed to create GLFW offscreen context for Utility driver");
    glfwTerminate();
    return nullptr;
  }

  gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

  return offscreen_context;
}


std::unordered_map<uint32_t, uint32_t> getMipLevelsFromUtilityDriver(IHRRenderDriver *driver, GLFWwindow* context)
{
  glfwPollEvents();
  RD_OGL32_Utility &utilityDrvRef = *(dynamic_cast<RD_OGL32_Utility *>(driver));
  glfwSwapBuffers(context);

  auto mipLevelsDict = utilityDrvRef.GetMipLevelsDict();

  /*for (std::pair<int32_t, int32_t> elem : mipLevelsDict)
    std::cout << " " << elem.first << ":" << elem.second << std::endl;*/

  glfwSetWindowShouldClose(context, GL_TRUE);

  return mipLevelsDict;
};