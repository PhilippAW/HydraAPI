//
// Created by vsan on 21.01.18.
//

#include <unistd.h>
#include <spawn.h>
#include <signal.h>
#include "HydraLegacyUtils.h"

struct HydraProcessLauncher : IHydraNetPluginAPI
{
  HydraProcessLauncher(const char* imageFileName, int width, int height, const char* connectionType, const std::vector<int>& a_devIdList, std::ostream* m_pLog = nullptr);
  ~HydraProcessLauncher();

  bool hasConnection() const override;

  void runAllRenderProcesses(RenderProcessRunParams a_params, const std::vector<HydaRenderDevice>& a_devList) override;
  void stopAllRenderProcesses() override;

protected:

  bool m_hydraServerStarted;
  std::ostream* m_pLog;


  std::vector<int>   m_mdDeviceList;
  std::vector<pid_t> m_mdProcessList;

  std::string m_connectionType;
  std::string m_imageFileName;

  int m_width;
  int m_height;
};


static std::ofstream g_logMain;

IHydraNetPluginAPI* CreateHydraServerConnection(int renderWidth, int renderHeight, bool inMatEditor, const std::vector<int>& a_devList)
{
  static int m_matRenderTimes = 0;

  IHydraNetPluginAPI* pImpl = nullptr;
  long ticks = sysconf(_SC_CLK_TCK);

  std::stringstream ss;
  ss << ticks;

  std::string imageName   = std::string("HydraHDRImage_") + ss.str();
  std::string messageName = std::string("HydraMessageShmem_") + ss.str();
  std::string guiName     = std::string("HydraGuiShmem_") + ss.str();

  std::ostream* logPtr = nullptr;

  if (!inMatEditor)
  {
    if (!g_logMain.is_open())
      g_logMain.open("/home/vsan/test/log.txt");
    logPtr = &g_logMain;
    pImpl  = new HydraProcessLauncher(imageName.c_str(), renderWidth, renderHeight, "main", a_devList, logPtr);
  }
  else // if in matEditor
  {

  }

  if (pImpl->hasConnection())
    return pImpl;
  else
  {
    delete pImpl;
    return nullptr;
  }

}



HydraProcessLauncher::HydraProcessLauncher(const char* imageFileName, int width, int height, const char* connectionType, const std::vector<int>& a_devIdList, std::ostream* a_pLog) :
        m_imageFileName(imageFileName), m_connectionType(connectionType), m_width(width), m_height(height), m_hydraServerStarted(false), m_pLog(a_pLog)
{
  m_mdDeviceList.clear();
  m_mdProcessList.clear();

  m_mdDeviceList = a_devIdList;
}

HydraProcessLauncher::~HydraProcessLauncher()
{
  stopAllRenderProcesses();
}


bool HydraProcessLauncher::hasConnection() const
{
  return true;
}

void CreateProcessUnix(const char* exePath, const char* allArgs, const bool a_debug, std::ostream* a_pLog, std::vector<pid_t>& a_mdProcessList)
{
  std::stringstream inStr(allArgs);
  std::vector<char*> cmd; // {"", "", NULL};
  
  std::string copyPath(exePath);
  cmd.push_back(const_cast<char*>(copyPath.c_str()));
  
  int i=0;
  while(!inStr.eof())
  {
    char* data = (char*)malloc(256);
    inStr >> data;
    cmd.push_back(data);
    i++;
  }
  
  cmd.push_back(nullptr);
  
  if (!a_debug)
  {
    auto pid = fork();
    
    switch (pid)
    {
      case -1:
        (*a_pLog) << "error forking hydraAPI" << std::endl;
        break;
      case 0: //child process
        (*a_pLog) << "before executing Hydra Core" << std::endl;
        execv(exePath, &cmd[0]);
        (*a_pLog) << "error launching or executing Hydra Core" << std::endl;
        exit(1);
      default:
        a_mdProcessList.push_back(pid);
        break;
    }
    
  }
  
  for (auto x : cmd)
    free(x);
  
}


void HydraProcessLauncher::runAllRenderProcesses(RenderProcessRunParams a_params, const std::vector<HydaRenderDevice>& a_devList)
{

  bool a_debug             = a_params.debug;

  const char* imageFileName = m_imageFileName.c_str();

  int width = m_width;
  int height = m_height;

  //m_mdProcessList.resize(a_devList.size());

  if (m_connectionType == "main")
  {

    char user_name[L_cuserid];
    cuserid(user_name);

    std::stringstream ss;
    ss << "/home/" << user_name << "/hydra/";

    std::string hydraPath = ss.str();
    if (a_params.customExePath != "")
      hydraPath = a_params.customExePath;

    if (!isFileExist(hydraPath.c_str()))
    {
      m_hydraServerStarted = false;
    }
    else
    {
      ss.str(std::string());
      ss << "-nowindow 1 ";
      ss << a_params.customExeArgs.c_str();
      if(!a_params.customLogFold.empty())
        ss << "-logdir \"" << a_params.customLogFold.c_str() << "\" ";


      std::string basicCmd = ss.str();

      m_hydraServerStarted = true;
      std::ofstream fout(hydraPath + "zcmd.txt");

      for (size_t i = 0; i < m_mdDeviceList.size(); i++)
      {
        int devId = m_mdDeviceList[i];

        ss.str(std::string());
        ss << " -cl_device_id " << devId;

        std::string cmdFull = basicCmd + ss.str();
        std::string hydraExe(hydraPath + "hydra");
  
        CreateProcessUnix(hydraExe.c_str(), cmdFull.c_str(), a_debug, m_pLog, m_mdProcessList);
        fout << cmdFull.c_str() << std::endl;
      }

      fout.close();

    }
  }
}

void HydraProcessLauncher::stopAllRenderProcesses()
{
  if (m_hydraServerStarted)
  {
    for (auto i = 0; i < m_mdProcessList.size(); i++)
    {
      if (m_mdProcessList[i] <= 0)
        continue;

      kill(m_mdProcessList[i], SIGKILL);
    }
  }
}
