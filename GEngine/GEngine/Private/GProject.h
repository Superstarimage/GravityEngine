#pragma once
#include "GRiInclude.h"




struct GStrPair
{
	std::wstring str1;
	std::wstring str2;

private:

	friend class boost::serialization::access;

	template<class Archive>
	void serialize(Archive & ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(str1);
		ar & BOOST_SERIALIZATION_NVP(str2);
	}
};

struct GProjectTextureInfo
{
	std::wstring UniqueFileName;
	bool bSrgb;

private:

	friend class boost::serialization::access;

	template<class Archive>
	void serialize(Archive & ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(UniqueFileName);
		ar & BOOST_SERIALIZATION_NVP(bSrgb);
	}
};

struct GProjectSceneObjectInfo
{
	std::wstring UniqueName = L"none";
	//std::wstring MaterialUniqueName = L"none";
	//std::map<std::wstring, std::wstring> OverrideMaterialUniqueName;
	std::list<GStrPair> OverrideMaterialUniqueName;
	std::wstring MeshUniqueName = L"none";
	float Location[3] = { 0.0f, 0.0f, 0.0f };
	float Rotation[3] = { 0.0f, 0.0f, 0.0f };
	float Scale[3] = { 1.0f, 1.0f, 1.0f };

private:

	friend class boost::serialization::access;

	template<class Archive>
	void serialize(Archive & ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(UniqueName);
		ar & BOOST_SERIALIZATION_NVP(OverrideMaterialUniqueName);
		ar & BOOST_SERIALIZATION_NVP(MeshUniqueName);
		ar & BOOST_SERIALIZATION_NVP(Location);
		ar & BOOST_SERIALIZATION_NVP(Rotation);
		ar & BOOST_SERIALIZATION_NVP(Scale);
	}
};

struct GProjectMeshInfo
{
	std::wstring MeshUniqueName = L"none";
	//std::map<std::wstring, std::wstring> MaterialUniqueName;
	std::list<GStrPair> MaterialUniqueName;
	int SdfResolution;
	std::list<float> Sdf;
	bool Save = false;

private:
	// 为了让串行化类库能够访问私有成员，所以声明了一个友元类
	friend class boost::serialization::access;

	template<class Archive>
	void serialize(Archive & ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(MeshUniqueName);
		ar & BOOST_SERIALIZATION_NVP(MaterialUniqueName);
		ar & BOOST_SERIALIZATION_NVP(SdfResolution);
		ar & BOOST_SERIALIZATION_NVP(Sdf);
	}
};

class GProject
{

public:

	GProject();
	~GProject();

	std::wstring mSkyCubemapUniqueName;					 // 天空盒
	std::list<GProjectTextureInfo> mTextureInfo;		 // 纹理信息
	std::list<GProjectSceneObjectInfo> mSceneObjectInfo; // 场景物体信息
	std::list<GProjectMeshInfo> mMeshInfo;				 // 网格信息

	void SaveProject(
		std::wstring filename,
		std::wstring skyCubemapUniqueName,
		std::unordered_map<std::wstring, std::unique_ptr<GRiTexture>>& pTextures,
		std::vector<GRiSceneObject*>& pSceneObjects,
		std::unordered_map<std::wstring, std::unique_ptr<GRiMesh>>& pMeshes
	)
	{
		mSkyCubemapUniqueName = skyCubemapUniqueName;

		mTextureInfo.clear(); // 清空场景对象的纹理列表

		for (auto it = pTextures.begin(); it != pTextures.end(); it++)
		{
			GProjectTextureInfo tInfo;
			tInfo.UniqueFileName = (*it).second->UniqueFileName; // first -> key value; second -> mapped value
			tInfo.bSrgb = (*it).second->bSrgb;
			mTextureInfo.push_back(tInfo);
		}

		mSceneObjectInfo.clear(); // 清空场景对象的物体信息列表

		for (auto i = 0u; i < pSceneObjects.size(); i++)
		{
			GProjectSceneObjectInfo soInfo;
			soInfo.UniqueName = pSceneObjects[i]->UniqueName; // 物体Unique名称，UniqueName是哈希表查找的唯一名称
			soInfo.MeshUniqueName = pSceneObjects[i]->GetMesh()->UniqueName;
			//soInfo.MaterialUniqueName = pSceneObjects[i]->GetMaterial()->UniqueName;
			soInfo.OverrideMaterialUniqueName.clear(); // 清空材质名称列表
			auto ovrdMatNames = pSceneObjects[i]->GetOverrideMaterialNames(); // 获取场景物体身上的多个材质哈希name map<name, UniqueName>
			for (auto pair : ovrdMatNames) // 可能有多对材质，所以循环处理
			{
				GStrPair pr;
				pr.str1 = pair.first; // 项目中材质的名称（为了与项目中其他材质做区分）
				pr.str2 = pair.second;// 材质的UniqueName，用于查找哈希表
				soInfo.OverrideMaterialUniqueName.push_back(pr);
			}
			std::vector<float> loc = pSceneObjects[i]->GetLocation();
			soInfo.Location[0] = loc[0];
			soInfo.Location[1] = loc[1];
			soInfo.Location[2] = loc[2];
			std::vector<float> rot = pSceneObjects[i]->GetRotation();
			soInfo.Rotation[0] = rot[0];
			soInfo.Rotation[1] = rot[1];
			soInfo.Rotation[2] = rot[2];
			std::vector<float> scale = pSceneObjects[i]->GetScale();
			soInfo.Scale[0] = scale[0];
			soInfo.Scale[1] = scale[1];
			soInfo.Scale[2] = scale[2];
			mSceneObjectInfo.push_back(soInfo);
		}

		mMeshInfo.clear(); // 清空网格信息对象的列表，Mesh可以理解为场景中的模型，它带有网格、材质和SDF

		for (auto it = pMeshes.begin(); it != pMeshes.end(); it++) // 模型 + 子模型材质 + SDF + SDF Resolution
		{
			GProjectMeshInfo mInfo;
			mInfo.MeshUniqueName = (*it).second->UniqueName;
			mInfo.MaterialUniqueName.clear(); // 清空材质列表
			for (auto& submesh : (*it).second->Submeshes)
			{
				GStrPair pr;
				pr.str1 = submesh.first; // 子Mesh名称
				pr.str2 = submesh.second.GetMaterial()->UniqueName; // 子Mesh上的材质名称
				mInfo.MaterialUniqueName.push_back(pr);

				//mInfo.MaterialUniqueName[submesh.first] = submesh.second.GetMaterial()->UniqueName;
			}

			auto pSdf = (*it).second->GetSdf(); // 一个float类型的vector，不过是用智能指针shared_ptr创建的，不必操心delete
			if (pSdf != nullptr)
			{
				auto pSdfData = pSdf->data(); // 返回vector所指数组内存的第一个元素的指针
				auto SdfSize = (int)pSdf->size();
				mInfo.Sdf.clear(); // 清空sdf列表
				for (int i = 0; i < SdfSize; i++)
				{
					mInfo.Sdf.push_back(pSdfData[i]); // 数组拷贝
				}
			}
			else
			{
				mInfo.Sdf.clear();
				mInfo.Sdf.push_back(-1);
			}
			mInfo.SdfResolution = (*it).second->GetSdfResolution();
			mInfo.Save = true;

			mMeshInfo.push_back(mInfo);
		}

		std::ofstream ofs;  // 建立输出文件流对象
		//ofs.open(filename, std::ios_base::out | std::ios_base::binary);
		ofs.open(filename); // 根据文件路径打开文件
		if (ofs.good())		// 检查文件是否打开成功（是否有逻辑、读写等错误）
		{
			{
				//boost::archive::binary_woarchive oa(ofs);
				boost::archive::xml_oarchive oa(ofs); // 创建文件输出归档类，根据ostream ofs来构造
				//boost::archive::text_oarchive oa(ofs);

				oa << boost::serialization::make_nvp("Project", *this); // 保存当前类对象
			}     
			ofs.close(); // 关闭文件
		}

	}

	// 根据场景文件.gproj（描述）载入场景
	void LoadProject(std::wstring filename)
	{
		std::ifstream ifs;  // 建立输入文件流对象
		ifs.open(filename); // 根据文件路径打开文件
		if (ifs.good()) {   // 检查文件是否打开成功（1. 输入操作符是否到达文件末尾；2. 是否有逻辑、读写等错误）
			{
				try
				{
					//boost::archive::binary_wiarchive ia(ifs);
					boost::archive::xml_iarchive ia(ifs); //input archive（存档），loading, file to memery
					//boost::archive::text_iarchive ia(ifs);
					try
					{
						ia >> boost::serialization::make_nvp("Project", *this); // 指定XML中标记节点名称为"Project"
					}
					catch (boost::archive::archive_exception const& e)
					{
						std::string eMessage(e.what());
						auto wErrorMessage = L"Fail to load project file.\n" + GGiEngineUtil::StringToWString(eMessage);
						MessageBox(nullptr, wErrorMessage.c_str(), L"Other Exception", MB_OK);
						ifs.close();
						return;
					}
				}
				catch (std::exception& e)
				{
					std::string eMessage(e.what());
					auto wErrorMessage = L"Fail to create archive. Project file may be empty or out of date.\n" + GGiEngineUtil::StringToWString(eMessage);
					MessageBox(nullptr, wErrorMessage.c_str(), L"Other Exception", MB_OK);
					ifs.close();
					return;
				}
			} 
			ifs.close(); // 把ifs相关联的文件关闭
		}
	}

private:

	// 为了能让串行化类库有权限访问私有成员，所以要声明一个友元类
	friend class boost::serialization::access;

	// 串行化的函数，这个函数完成对象的保存与恢复
	template<class Archive>
	void serialize(Archive & ar, const unsigned int version)
	{
		ar & BOOST_SERIALIZATION_NVP(mSkyCubemapUniqueName);
		ar & BOOST_SERIALIZATION_NVP(mTextureInfo);
		ar & BOOST_SERIALIZATION_NVP(mSceneObjectInfo);
		ar & BOOST_SERIALIZATION_NVP(mMeshInfo);
	}

};

