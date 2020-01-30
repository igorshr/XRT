/**
 * Copyright (C) 2019 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <string>
#include <iostream>
#include <fstream>
#include <climits>
#include <getopt.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "xbmgmt.h"
#include "core/pcie/linux/scan.h"

#define SYSFS_PATH      "/sys/bus/pci/devices"
#define XILINX_VENDOR   "0x10ee"
#define XILINX_US       "0x9134"
#define POLL_TIMEOUT    60      /* Set a pool timeout as 60sec */

static int hotplugRescan(void);
static int removeDevice(unsigned int index, bool is_userpf);
static int shutdownDevice(unsigned int index, bool is_userpf);

const char *subCmdHotplugDesc = "Perform managed hotplug on the xilinx device";
const char *subCmdHotplugUsage = "--offline bdf | --online";

int hotplugHandler(int argc, char *argv[])
{
    sudoOrDie();

    if (argc < 2)
        return -EINVAL;
    
    int ret = 0;
    unsigned int index = UINT_MAX;
    int isRemove = 0;
    int isRescan = 0;
    bool is_userpf = false;

    const static option opts[] = {
        { "offline", required_argument, nullptr, '0' },
        { "online", no_argument, nullptr, '1' },
        { nullptr, 0, nullptr, 0 },
    };

    while (true) 
    {
        const auto opt = getopt_long(argc, argv, "", opts, nullptr);

        if (opt == -1)
            break;

        switch (opt) 
        {
            case '0':
                index = bdf2index(optarg);
                if (index == UINT_MAX)
                    return -ENOENT;
               
                isRemove = 1; 
                break;

            case '1':
                isRescan = 1;
                break;

            default:
                return -EINVAL;
        } 
    }
    
    /* Get permission from user. */
    std::cout << "CAUTION: Performing hotplug command. " <<
        "This command is going to impact both user pf and mgmt pf.\n" <<
        "Please make sure no application is currently running." << std::endl;

    if(!canProceed())
        return -ECANCELED;

    if (isRemove)
    {
        /* Shutdown user_pf before trigger hot removal*/
        is_userpf = true;
        ret = shutdownDevice(index, is_userpf); 
        if (ret)
        {
            if (ret == -ENOENT)
            {
                std::cout << "INFO: Device entry doesn't exists. If you are running on VM Environment, \n" <<
                   "Please shutdown the VM before performing this operation.\n" << std::endl;
                return 0;
            }
            else
            {
                std::cout << "Device Shutdown failed." << std::endl;
                return -EINVAL;
            }
        }

        /* Remove user_pf */
        is_userpf = true;
        ret = removeDevice(index, is_userpf);
        if (ret)
            return ret;

        /* Remove mgmt_pf */
        is_userpf = false;
        ret = removeDevice(index, is_userpf);
        if (ret)
            return ret;
    }

    if (isRescan)
    {
        /* Rescan from /sys/bus/pci/<Root Port>/rescan */
        ret = hotplugRescan();
        if (ret)
            return ret;
    }

    return ret;
}

static int shutdownDevice(unsigned int index, bool is_userpf)
{
    std::string errmsg;
    int shutdownStatus = -EINVAL;
    int wait  = 0;
    auto dev = pcidev::get_dev(index, is_userpf);

    /* "echo 1 > /sys/bus/pci/<EndPoint>/shutdown" to trigger shutdown of the device */
    std::string path = dev->get_sysfs_path("", "shutdown");
    if (!boost::filesystem::exists(path)) 
        return -ENOENT;

    dev->sysfs_put("", "shutdown", errmsg, "1");
    if (!errmsg.empty())
    {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    /* Poll till shutdown is done */
    do {
        sleep(1);
        dev->sysfs_get<int>("", "shutdown", errmsg, shutdownStatus, EINVAL);
        if (!errmsg.empty())
        {
            std::cout << errmsg << std::endl;
            return -EINVAL;
        }

        if (shutdownStatus == 1){
            /* Shutdown is done successfully. Returning from here */
            return 0;
        }

    } while (++wait < POLL_TIMEOUT );

    return -ETIMEDOUT;
}

static int removeDevice(unsigned int index, bool is_userpf)
{
    std::string errmsg;
    auto dev = pcidev::get_dev(index, is_userpf);

    /* "echo 1 > /sys/bus/pci/<EndPoint>/remove" to trigger hot remove of the device */
    dev->sysfs_put("", "remove", errmsg, "1");
    if (!errmsg.empty())
    {
        std::cout << errmsg << std::endl;
        return -EINVAL;
    }

    return 0;
}

static boost::filesystem::path findXilinxRootPort(void)
{
    boost::filesystem::path rootPortPath;
    std::string vendor_id, device_id;

    for (auto file = boost::filesystem::directory_iterator(SYSFS_PATH); 
            file != boost::filesystem::directory_iterator(); file++)
    {
        rootPortPath = file->path();
        for (auto jfile = boost::filesystem::directory_iterator(rootPortPath); 
                jfile != boost::filesystem::directory_iterator(); jfile++)
        {
            boost::filesystem::path dirName = jfile->path();
            if (!boost::filesystem::is_directory(dirName))
                continue;

            try 
            {
                boost::filesystem::path vendorPath = dirName;
                vendorPath /= "vendor";
                if (boost::filesystem::exists(vendorPath))
                {
                    boost::filesystem::ifstream file(vendorPath);
                    file >> vendor_id;
                    file.close();
                }

                boost::filesystem::path devicePath = dirName;
                devicePath /= "device";
                if (boost::filesystem::exists(devicePath))
                {
                    boost::filesystem::ifstream file(devicePath);
                    file >> device_id;
                    file.close();
                }
            }

            catch (const boost::filesystem::ifstream::failure& e)
            {
                continue; 
            }
        
            if (!vendor_id.compare(XILINX_VENDOR) && !device_id.compare(XILINX_US))
                return rootPortPath;
        }

        rootPortPath = "";
    }

    return rootPortPath;
}

static int hotplugRescan(void)
{
    boost::filesystem::ofstream ofile;     
    boost::filesystem::path sysfs_path = findXilinxRootPort();

    sysfs_path /= "rescan";
    if (sysfs_path.empty()) {
        return -ENOENT;
    }

    try 
    {
        ofile.open(sysfs_path);
        if (!ofile.is_open()) {
            std::cout << "Failed to open " << sysfs_path << ":" << strerror(errno) << std::endl;
            return -errno;
        }

        /* "echo 1 > /sys/bus/pci/<Root Port>/rescan" to trigger the rescan for hot plug devices */ 
        ofile << 1;
        ofile.flush();
        if (!ofile.good()) {
            std::cout << "Failed to write " << sysfs_path << ":"  << strerror(errno) << std::endl;
            ofile.close();
            return -errno;
        }
    }
    
    catch (const boost::filesystem::ifstream::failure& err) {
        std::cout << "Exception!!!! " << err.what();
        ofile.close();
        return -errno;
    }
        
    ofile.close();

    return 0;
}


