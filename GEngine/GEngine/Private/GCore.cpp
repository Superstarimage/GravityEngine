#pragma once
#include "stdafx.h"
#include "GCore.h"
#include <WindowsX.h>
#include <io.h>
#include "GDxFloat4x4.h"


enum DebugLayerMask
{
	RGB,
	sRGB,
	R,
	G,
	B,
	RG
};

#pragma region Class

GCore::GCore()
{
	mTimer = std::make_unique<GGiGameTimer>();
	mRenderer = &GDxRenderer::GetRenderer();
	mRenderer->SetTimer(mTimer.get());
	mProject = new GProject();
	mGuiCallback = new GGuiCallback();
}

GCore::~GCore()
{
#ifdef USE_IMGUI
	mImgui->ShutDown();
#endif
}

GCore& GCore::GetCore()
{
	static GCore *instance = new GCore();
	return *instance;
}

#pragma endregion

#pragma region Main

void GCore::Run()
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	try
	{
		try
		{
			try
			{
				MSG msg = { 0 };

				mTimer->Reset();

				while (msg.message != WM_QUIT)
				{
					// If there are Window messages then process them.
					if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
					{
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
					// Otherwise, do animation/game stuff.
					else
					{
						mTimer->Tick();

						if (!mAppPaused)
						{
							mRenderer->CalculateFrameStats();
							Update();
							mRenderer->Draw(mTimer.get());

							RecordPrevFrame(mTimer.get());
						}
						else
						{
							Sleep(100);
						}
					}
				}
			}
			catch (DxException& e)
			{
				MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
			}
		}
		catch (GGiException& e)
		{
			MessageBox(nullptr, e.GetErrorMessage().c_str(), L"Engine-defined Exception", MB_OK);
		}
	}
	catch (std::exception& e)
	{
		std::string msg(e.what());
		MessageBox(nullptr, GGiEngineUtil::StringToWString(msg).c_str(), L"Other Exception", MB_OK);
	}
}

void GCore::Initialize(HWND OutputWindow, double width, double height)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	try
	{
		try
		{
			try
			{
				// 预初始化（初始化D3D对象、创建渲染器工厂对象、创建BFX管理器、初始化OnResize代码、重置命令队列）
				mRenderer->PreInitialize(OutputWindow, width, height); 

				LoadProject(); // 载入场景（实际上是根据xml文件内容填充GProject对象，填充了之后有什么用呢？）

				// RendererFactory包含成员函数：创建材质队形、创建材质加载器对象、创建材质对象、创建Mesh对象、创建几何体生成器对象、创建场景物体对象、创建Imgui对象
				pRendererFactory = mRenderer->GetFactory(); // 获取渲染工厂
				CreateImgui();   // 创建Imgui 
				LoadTextures();  // 加载纹理
				mRenderer->SyncTextures(mTextures);   // 同步纹理，将mTextures中数据转存到GRiRenderer.h中的pTextures（字典）中
				LoadMaterials(); // 加载材质
				mRenderer->SyncMaterials(mMaterials); // 同步材质，将mMaterials中数据转存到GRiRenderer.h中的pMaterials（字典）中
				LoadMeshes();	 // 加载网格
				mRenderer->SyncMeshes(mMeshes);		  // 同步网格，将mMeshes中数据转存到GRiRenderer.h中的pMeshes（字典）中
				LoadSceneObjects(); // 加载场景对象
				mRenderer->SyncSceneObjects(mSceneObjects, mSceneObjectLayer); // 同步场景中对象
				LoadCameras();   // 加载摄像机
				std::vector<GRiCamera*> cam =
				{
					mCamera.get(),
					mCubemapSampleCamera[0].get(), // CubeMap
					mCubemapSampleCamera[1].get(),
					mCubemapSampleCamera[2].get(),
					mCubemapSampleCamera[3].get(),
					mCubemapSampleCamera[4].get(),
					mCubemapSampleCamera[5].get()
				};
				mRenderer->SyncCameras(cam); // 同步摄像机

				mRenderer->Initialize(); // 初始化
			}
			catch (DxException& e)
			{
				MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
			}
		}
		catch (GGiException& e)
		{
			MessageBox(nullptr, e.GetErrorMessage().c_str(), L"Engine-defined Exception", MB_OK);
		}
	}
	catch (std::exception& e)
	{
		std::string msg(e.what());
		MessageBox(nullptr, GGiEngineUtil::StringToWString(msg).c_str(), L"Other Exception", MB_OK);
	}
}

void GCore::Update()
{
	OnKeyboardInput(mTimer.get());

#ifdef USE_IMGUI
	UpdateGui(mTimer.get());
#endif

	GGiCpuProfiler::GetInstance().BeginFrame();

	mRenderer->Update(mTimer.get());
}

// WPF+Winform GUI运行后执行的消息循环
void GCore::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{

#ifdef USE_IMGUI
	ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
#endif

	switch (msg)
	{
		// WM_ACTIVATE is sent when the window is activated or deactivated.
		// We pause the game when the window is deactivated and unpause it
		// when it becomes active.
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			// 用于帧率计算
			mAppPaused = true;
			mTimer->Stop();
		}
		else
		{
			// 用于帧率计算
			mAppPaused = false;
			mTimer->Start();
		}
		return; 0;

		// 用户调整窗口尺寸时会持续出现WM_SIZE消息
		// WM_SIZE is sent when the user resizes the window.
	case WM_SIZE:
		// 保存新客户端的尺寸
		// Save the new client area dimensions.
		mRenderer->SetClientWidth(LOWORD(lParam));
		mRenderer->SetClientHeight(HIWORD(lParam));
		if (mRenderer->IsRunning())
		{
			if (wParam == SIZE_MINIMIZED) // 窗口最小化
			{
				mAppPaused = true;
				mMinimized = true;
				mMaximized = false;
			}
			else if (wParam == SIZE_MAXIMIZED) // 窗口最大化
			{
				mAppPaused = false;
				mMinimized = false;
				mMaximized = true;
				OnResize();
			}
			else if (wParam == SIZE_RESTORED)
			{

				// Restoring from minimized state?
				if (mMinimized)
				{
					mAppPaused = false;
					mMinimized = false;
					OnResize();
				}

				// Restoring from maximized state?
				else if (mMaximized)
				{
					mAppPaused = false;
					mMaximized = false;
					OnResize();
				}
				else if (mResizing)
				{
					// If user is dragging the resize bars, we do not resize
					// the buffers here because as the user continuously
					// drags the resize bars, a stream of WM_SIZE messages are
					// sent to the window, and it would be pointless (and slow)
					// to resize for each WM_SIZE message received from dragging
					// the resize bars.  So instead, we reset after the user is
					// done resizing the window and releases the resize bars, which
					// sends a WM_EXITSIZEMOVE message.
				}
				else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
				{
					OnResize();
				}
			}
		}
		return; 0;

		// 当用户抓取调整栏时发送WM_ENTERSIZEMOVE消息
		// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
	case WM_ENTERSIZEMOVE:
		mAppPaused = true;
		mResizing = true;
		mTimer->Stop();
		return; 0;

		// 当用户释放调整栏时发送WM_EXITSIZEMOVE消息
	// 此处将根据新的窗口大小重置相关对象（如缓冲区、视图等）
		// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
		// Here we reset everything based on the new window dimensions.
	case WM_EXITSIZEMOVE:
		mAppPaused = false;
		mResizing = false;
		mTimer->Start();
		OnResize();
		return; 0;

		// 当窗口被销毁时发送WM_DESTROY消息
		// WM_DESTROY is sent when the window is being destroyed.
	case WM_DESTROY:
		PostQuitMessage(0);
		return; 0;

		// 当某一菜单处于激活状态，而且用户按下的既不是助记键（mnemonic key）
		// 也不是加速键（acceleratorkey）时，就发送WM_MENUCHAR消息
		// The WM_MENUCHAR message is sent when a menu is active and the user presses
		// a key that does not correspond to any mnemonic or accelerator key.
	case WM_MENUCHAR:
		// 当按下组合键alt-enter时不发出beep嗡鸣声
		// Don't beep when we alt-enter.
		return; MAKELRESULT(0, MNC_CLOSE);

		// 捕获此消息以防止窗口变得过小
		// Catch this message so to prevent the window from becoming too small.
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
		((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
		return; 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return; 0;
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return; 0;
	case WM_MOUSEMOVE:
		OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return; 0;
	case WM_KEYUP:
		if (wParam == VK_ESCAPE)
		{
			;//PostQuitMessage(0);
		}

		return; 0;
	}
}

#pragma endregion

#pragma region Input

void GCore::OnKeyboardInput(const GGiGameTimer* gt)
{
	const float dt = gt->DeltaTime();
	
	if ((GetAsyncKeyState('W') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Walk(mCameraSpeed * dt);

	if ((GetAsyncKeyState('S') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Walk(-mCameraSpeed * dt);

	if ((GetAsyncKeyState('D') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Strafe(mCameraSpeed * dt);

	if ((GetAsyncKeyState('A') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Strafe(-mCameraSpeed * dt);

	if ((GetAsyncKeyState('E') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Ascend(mCameraSpeed * dt);

	if ((GetAsyncKeyState('Q') & 0x8000) && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
		mCamera->Ascend(-mCameraSpeed * dt);

	//mCamera->UpdateViewMatrix();
}

void GCore::OnResize()
{
	mCamera->SetLens(FOV_Y, mRenderer->AspectRatio(), NEAR_Z, FAR_Z);
	mRenderer->OnResize();
}

void GCore::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	// 对被点击的窗口设置鼠标捕获，此后所有的鼠标事件都将针对该窗口
	SetCapture(mRenderer->MainWnd());

	// ImGuizmo::IsOver() return true if mouse cursor is over any gizmo control (axis, plan or screen component)

#ifdef USE_IMGUI
	if ((btnState & MK_LBUTTON) != 0 && mboolSceneLoad == true && !ImGuizmo::IsOver())
#else
	if ((btnState & MK_LBUTTON) != 0)
#endif
	{
		Pick(x, y);
	}

	// Hide mouse cursor.
	if ((btnState & MK_RBUTTON) != 0)
	{
		while (ShowCursor(false) >= 0);
	}
}

void GCore::OnMouseUp(WPARAM btnState, int x, int y)
{
	// 对已经点击的窗口释放鼠标捕获，恢复通常的鼠标输入处理
	ReleaseCapture();

	// Show mouse cursor.
	if (!(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
	{
		while (ShowCursor(true) < 0);
	}
}

void GCore::OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_RBUTTON) != 0)
	{
		// Make each pixel correspond to a quarter of a degree.
		float dx = 0.25f*static_cast<float>(x - mLastMousePos.x) * GGiEngineUtil::PI / 180.0f;
		float dy = 0.25f*static_cast<float>(y - mLastMousePos.y) * GGiEngineUtil::PI / 180.0f;

		mCamera->Pitch(dy);
		mCamera->RotateY(dx);
	}

	mLastMousePos.x = x;
	mLastMousePos.y = y;
}

#pragma endregion

#pragma region Initialization

void GCore::CreateImgui()
{
	GRiImgui* pImgui = pRendererFactory->CreateImgui();
	mImgui.reset(pImgui);
	mRenderer->SetImgui(mImgui.get());
}

// 导入纹理资源
void GCore::LoadTextures()
{
	std::vector<std::wstring> format;
	format.emplace_back(L"dds");
	format.emplace_back(L"png");
	format.emplace_back(L"tga");
	format.emplace_back(L"jpg");

	// 获取指定文件夹relPath中指定格式format的所有文件的相对路径，并存放到vector中
	// move将右值变成左值，如果move右边是是个字符串变量，move操作后，字符串将为空
	std::vector<std::wstring> files = std::move(GetAllFilesInFolder(L"Content", true, format)); 
	
	std::unique_ptr<GRiTextureLoader> textureLoader(pRendererFactory->CreateTextureLoader()); // 创建材质加载器

	for (auto file : files) // 对于每一个材质文件
	{
		bool bSrgb = false;
		for (auto texInfo : mProject->mTextureInfo) // 遍历场景类对象中的纹理信息列表（每个元素中包含uniquefilename和bsrgb）
		{
			if (file == texInfo.UniqueFileName) // 用实际读取到的材质文件查找场景档案中对应的材质文件
			{
				bSrgb = texInfo.bSrgb;
				break;
			}
		}
		GRiTexture* tex = textureLoader->LoadTexture(WorkDirectory, file, bSrgb); // 加载材质
		std::wstring texName = tex->UniqueFileName; // 文件的相对路径
		std::unique_ptr<GRiTexture> temp(tex);
		mTextures[texName] = std::move(temp);
	}

	// 创建默认纹理
	files.clear();
	files.push_back(L"Resource\\Textures\\GE_Default_Albedo.png");
	files.push_back(L"Resource\\Textures\\GE_Default_Normal.png");
	files.push_back(L"Resource\\Textures\\GE_Default_Orm.png");
	files.push_back(L"Resource\\Textures\\GE_DefaultTexture_Albedo.png");
	files.push_back(L"Resource\\Textures\\GE_DefaultTexture_Normal.png");
	files.push_back(L"Resource\\Textures\\GE_DefaultTexture_Orm.png");
	files.push_back(L"Resource\\Textures\\IBL_BRDF_LUT.png");
	files.push_back(L"Resource\\Textures\\BlueNoise.png");
	for (auto file : files)
	{
		bool bSrgb = false;
		if (file == L"Resource\\Textures\\GE_Default_Albedo.png" || file == L"Resource\\Textures\\GE_DefaultTexture_Albedo.png")
			bSrgb = true;
		GRiTexture* tex = textureLoader->LoadTexture(EngineDirectory, file, bSrgb);
		std::wstring texName = tex->UniqueFileName;
		std::unique_ptr<GRiTexture> temp(tex);
		mTextures[texName] = std::move(temp);
	}

	//
	// 加载天空立方体贴图 Load sky cubemap.
	//
	// 加载默认立方体贴图
	mSkyCubemapUniqueName = mProject->mSkyCubemapUniqueName;
	if (mSkyCubemapUniqueName.find(L"Resource") != 0 && mTextures.find(mSkyCubemapUniqueName) == mTextures.end())
		mSkyCubemapUniqueName = GetDefaultSkyCubemapUniqueName();

	GRiTexture* tex;
	if (mSkyCubemapUniqueName.find(L"Resource") == 0)
		tex = textureLoader->LoadTexture(EngineDirectory, mSkyCubemapUniqueName, true);
	else
		tex = textureLoader->LoadTexture(WorkDirectory, mSkyCubemapUniqueName, true);
	std::wstring texName = L"skyCubeMap";
	std::unique_ptr<GRiTexture> temp(tex);
	mTextures[texName] = std::move(temp);
}

// 加载材质资源
void GCore::LoadMaterials()
{
	mMaterialIndex = 0;

	auto defaultMat = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	defaultMat->UniqueName = L"Default";
	defaultMat->Name = L"Default";
	defaultMat->MatIndex = mMaterialIndex++;
	defaultMat->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Albedo.png"].get());
	defaultMat->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Normal.png"].get());
	defaultMat->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Orm.png"].get());
	mMaterials[L"Default"] = std::move(defaultMat);

	auto debug_albedo = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_albedo->UniqueName = L"debug_albedo";
	debug_albedo->Name = L"debug_albedo";
	debug_albedo->MatIndex = mMaterialIndex++;
	debug_albedo->AddScalar(0.01f);//Albedo
	debug_albedo->AddScalar((float)DebugLayerMask::sRGB);//RGB
	mMaterials[L"debug_albedo"] = std::move(debug_albedo);

	auto debug_normal = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_normal->UniqueName = L"debug_normal";
	debug_normal->Name = L"debug_normal";
	debug_normal->MatIndex = mMaterialIndex++;
	debug_normal->AddScalar(1.01f);//Normal
	debug_normal->AddScalar((float)DebugLayerMask::RGB);//RGB
	mMaterials[L"debug_normal"] = std::move(debug_normal);

	auto debug_worldpos = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_worldpos->UniqueName = L"debug_worldpos";
	debug_worldpos->Name = L"debug_worldpos";
	debug_worldpos->MatIndex = mMaterialIndex++;
	debug_worldpos->AddScalar(2.01f);//WorldPos
	debug_worldpos->AddScalar((float)DebugLayerMask::RGB);//RGB
	mMaterials[L"debug_worldpos"] = std::move(debug_worldpos);

	auto debug_velocity = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_velocity->UniqueName = L"debug_velocity";
	debug_velocity->Name = L"debug_velocity";
	debug_velocity->MatIndex = mMaterialIndex++;
	debug_velocity->AddScalar(2.01f);//Velocity
	debug_velocity->AddScalar((float)DebugLayerMask::RG);//RedGreen
	mMaterials[L"debug_velocity"] = std::move(debug_velocity);

	auto debug_roughness = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_roughness->UniqueName = L"debug_roughness";
	debug_roughness->Name = L"debug_roughness";
	debug_roughness->MatIndex = mMaterialIndex++;
	debug_roughness->AddScalar(3.01f);//OcclusionRoughnessMetallic
	debug_roughness->AddScalar((float)DebugLayerMask::G);//Green
	mMaterials[L"debug_roughness"] = std::move(debug_roughness);

	auto debug_metallic = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	debug_metallic->UniqueName = L"debug_metallic";
	debug_metallic->Name = L"debug_metallic";
	debug_metallic->MatIndex = mMaterialIndex++;
	debug_metallic->AddScalar(3.01f);//OcclusionRoughnessMetallic
	debug_metallic->AddScalar((float)DebugLayerMask::B);//Blue
	mMaterials[L"debug_metallic"] = std::move(debug_metallic);

	auto sky = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	sky->UniqueName = L"sky";
	sky->Name = L"sky";
	sky->MatIndex = mMaterialIndex++;
	sky->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Albedo.png"].get());//Diffuse
	sky->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Normal.png"].get());//Normal
	mMaterials[L"sky"] = std::move(sky);

	// 从文件中载入所有的材质文件
	// Load materials from file.
	{
		std::vector<std::wstring> format;
		format.emplace_back(L"gmat");
		std::vector<std::wstring> files = std::move(GetAllFilesInFolder(L"Content", true, format));

		for (auto file : files)
		{
			auto newMat = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());

			auto matFile = std::make_unique<GMaterial>(newMat.get());
			matFile->UniqueName = file;
			matFile->LoadMaterial(WorkDirectory);      // 从xml加载材质信息
			mMaterialFiles[file] = std::move(matFile); 

			newMat->UniqueName = file;
			newMat->Name = GGiEngineUtil::GetFileName(file);
			newMat->SetScale(mMaterialFiles[file]->MaterialScale[0], mMaterialFiles[file]->MaterialScale[1]);
			newMat->MatIndex = mMaterialIndex++;

			for (auto txtName : mMaterialFiles[file]->TextureNames)
			{
				if (mTextures.find(txtName) != mTextures.end())
				{
					newMat->AddTexture(mTextures[txtName].get());
				}
				else
				{
					newMat->AddTexture(mTextures[L"Resource\\Textures\\GE_Default_Albedo.png"].get());
				}
			}

			for (auto scalar : mMaterialFiles[file]->ScalarParams)
			{
				newMat->AddScalar(scalar);
			}

			std::list<float>::iterator iter = mMaterialFiles[file]->VectorParams.begin();
			for (auto i = 0u; i < (UINT)(mMaterialFiles[file]->VectorParams.size() / 4); i++)
			{
				GGiVector4 vec;
				float x = *iter;
				iter++;
				float y = *iter;
				iter++;
				float z = *iter;
				iter++;
				float w = *iter;
				iter++;
				vec = GGiMath::GGiVectorSet(x, y, z, w);
				newMat->AddVector(vec);
			}

			mMaterialFiles[file]->LoadMaterialData();

			mMaterials[file] = std::move(newMat);
		}
	}
}
 
void GCore::LoadMeshes()
{

	GRiGeometryGenerator* geoGen = pRendererFactory->CreateGeometryGenerator(); // 创建几何体生成器

	std::vector<GRiMeshData> meshData; // Mesh向量列表
	GRiMeshData boxMeshData = geoGen->CreateBox(40.0f, 40.0f, 40.0f, 3);
	meshData.push_back(boxMeshData);
	auto geo = pRendererFactory->CreateMesh(meshData); // 把网格信息交给D3D去创建Mesh
	geo->UniqueName = L"Box";
	geo->Name = L"Box";
	geo->Submeshes[L"Box"].SetMaterial(mMaterials[L"Default"].get()); // 设置材质
	std::unique_ptr<GRiMesh> temp1(geo);
	mMeshes[geo->UniqueName] = std::move(temp1);

	meshData.clear();
	GRiMeshData gridMeshData = geoGen->CreateGrid(40.0f, 40.0f, 40, 40);
	meshData.push_back(gridMeshData);
	geo = pRendererFactory->CreateMesh(meshData);
	geo->UniqueName = L"Grid";
	geo->Name = L"Grid";
	geo->Submeshes[L"Grid"].SetMaterial(mMaterials[L"Default"].get());
	std::unique_ptr<GRiMesh> temp2(geo);
	mMeshes[geo->UniqueName] = std::move(temp2);

	meshData.clear();
	GRiMeshData sphereMeshData = geoGen->CreateSphere(20.0f, 20, 20);
	meshData.push_back(sphereMeshData);
	geo = pRendererFactory->CreateMesh(meshData);
	geo->UniqueName = L"Sphere";
	geo->Name = L"Sphere";
	geo->Submeshes[L"Sphere"].SetMaterial(mMaterials[L"Default"].get());
	std::unique_ptr<GRiMesh> temp3(geo);
	mMeshes[geo->UniqueName] = std::move(temp3);

	meshData.clear();
	GRiMeshData cylinderMeshData = geoGen->CreateCylinder(15.0f, 15.0f, 40.0f, 20, 20);
	meshData.push_back(cylinderMeshData);
	geo = pRendererFactory->CreateMesh(meshData);
	geo->UniqueName = L"Cylinder";
	geo->Name = L"Cylinder";
	geo->Submeshes[L"Cylinder"].SetMaterial(mMaterials[L"Default"].get());
	std::unique_ptr<GRiMesh> temp4(geo);
	mMeshes[geo->UniqueName] = std::move(temp4);

	meshData.clear();
#if USE_REVERSE_Z
	GRiMeshData quadMeshData = geoGen->CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
#else
	GRiMeshData quadMeshData = geoGen->CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
#endif
	meshData.push_back(quadMeshData);
	geo = pRendererFactory->CreateMesh(meshData);
	geo->UniqueName = L"Quad";
	geo->Name = L"Quad";
	geo->Submeshes[L"Quad"].SetMaterial(mMaterials[L"Default"].get());
	std::unique_ptr<GRiMesh> temp5(geo);
	mMeshes[geo->UniqueName] = std::move(temp5);

	std::vector<std::wstring> format;
	format.emplace_back(L"fbx");
	std::vector<std::wstring> files = std::move(GetAllFilesInFolder(L"Content", true, format)); // 查找fbx资源文件，返回它们的相对路径
	for (auto file : files)
	{
		meshData.clear();
		mRenderer->GetFilmboxManager()->ImportFbxFile_Mesh(WorkDirectory + file, meshData); // 获取fbx文件的网格信息，并存到meshData中
		geo = pRendererFactory->CreateMesh(meshData); // 把网格信息交给D3D去创建Mesh
		for (auto& subMesh : geo->Submeshes)
		{
			subMesh.second.SetMaterial(mMaterials[L"Default"].get());
		}
		geo->UniqueName = file;
		geo->Name = GGiEngineUtil::GetFileName(file);
		std::unique_ptr<GRiMesh> temp(geo);
		mMeshes[geo->UniqueName] = std::move(temp);
	}
	for (auto info : mProject->mMeshInfo)
	{
		if (mMeshes.find(info.MeshUniqueName) != mMeshes.end()) // 在资源文件中查找xml文件声明的文件
		{
			bool bFound = false;
			std::wstring matName;
			for (auto& submesh : mMeshes[info.MeshUniqueName]->Submeshes) // 针对mesh的submesh再次查找xml文件中声明的文件
			{
				bFound = false;
				for (auto it = info.MaterialUniqueName.begin(); it != info.MaterialUniqueName.end(); it++)
				{
					if ((*it).str1 == submesh.first)
					{
						bFound = true;
						matName = (*it).str2;

						// 【优化】 找到后是否可以直接break
					}
				}

				if (bFound)
				{
					if (mMaterials.find(matName) != mMaterials.end()) // 子mesh同样附有材质
					{
						submesh.second.SetMaterial(mMaterials[matName].get());
					}
				}
			}

			// 对从资源文件中读取的mesh文件设置xml文件中对应读取的sdfresolution描述
			mMeshes[info.MeshUniqueName]->SetSdfResolution(info.SdfResolution); 
			// 资源文件里有mesh文件，xml文件里有对该mesh文件sdfresolution的描述

			auto SdfSize = info.Sdf.size();
			std::vector<float> sdf;
			if (SdfSize > 1)
			{
				sdf.clear();
				for (auto iter = info.Sdf.begin(); iter != info.Sdf.end(); iter++)
				{
					sdf.push_back(*iter);
				}
				mMeshes[info.MeshUniqueName]->InitializeSdf(sdf);
			}
		}
	}
}

// 加载场景对象
void GCore::LoadSceneObjects()
{
	mboolSceneLoad = true; // 载入场景标识开关打开 Added by Ssi

	mSceneObjectIndex = 0; // 场景中物体的索引

	// 创建screen quads以进行光通过和后期处理
	// Create screen quads for light pass and post process.
	std::unique_ptr<GRiSceneObject> fullScreenQuadSO(pRendererFactory->CreateSceneObject());
	fullScreenQuadSO->UniqueName = L"FullScreenQuad";
	fullScreenQuadSO->UpdateTransform();
	fullScreenQuadSO->ResetPrevTransform();
	fullScreenQuadSO->SetTexTransform(GGiFloat4x4::Identity());
	fullScreenQuadSO->SetObjIndex(mSceneObjectIndex++);
	fullScreenQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"Default"].get());
	fullScreenQuadSO->SetMesh(mMeshes[L"Quad"].get());
	mSceneObjectLayer[(int)RenderLayer::ScreenQuad].push_back(fullScreenQuadSO.get());
	mSceneObjects[fullScreenQuadSO->UniqueName] = std::move(fullScreenQuadSO);

	std::unique_ptr<GRiSceneObject> skySO(pRendererFactory->CreateSceneObject());
	skySO->UniqueName = L"Sky";
	skySO->SetScale(5000.f, 5000.f, 5000.f);
	skySO->UpdateTransform();
	skySO->ResetPrevTransform();
	skySO->SetTexTransform(GGiFloat4x4::Identity());
	skySO->SetObjIndex(mSceneObjectIndex++);
	skySO->SetOverrideMaterial(L"Sphere", mMaterials[L"Default"].get());
	skySO->SetMesh(mMeshes[L"Sphere"].get());
	mSceneObjectLayer[(int)RenderLayer::Sky].push_back(skySO.get());
	mSceneObjects[skySO->UniqueName] = std::move(skySO);

	// Create debug quads.
	{
		std::unique_ptr<GRiSceneObject> albedoQuadSO(pRendererFactory->CreateSceneObject());
		albedoQuadSO->UniqueName = L"AlbedoQuad";
		albedoQuadSO->SetScale(.2f, .2f, .2f);
		albedoQuadSO->SetLocation(0.f, 0.f, 0.f);
		albedoQuadSO->UpdateTransform();
		albedoQuadSO->ResetPrevTransform();
		albedoQuadSO->SetTexTransform(GGiFloat4x4::Identity());
		albedoQuadSO->SetObjIndex(mSceneObjectIndex++);
		albedoQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"debug_albedo"].get());
		albedoQuadSO->SetMesh(mMeshes[L"Quad"].get());
		mSceneObjectLayer[(int)RenderLayer::Debug].push_back(albedoQuadSO.get());
		mSceneObjects[albedoQuadSO->UniqueName] = std::move(albedoQuadSO);

		std::unique_ptr<GRiSceneObject> normalQuadSO(pRendererFactory->CreateSceneObject());
		normalQuadSO->UniqueName = L"NormalQuad";
		normalQuadSO->SetScale(.2f, .2f, .2f);
		normalQuadSO->SetLocation(.2f, 0.f, 0.f);
		normalQuadSO->UpdateTransform();
		normalQuadSO->ResetPrevTransform();
		normalQuadSO->SetTexTransform(GGiFloat4x4::Identity());
		normalQuadSO->SetObjIndex(mSceneObjectIndex++);
		normalQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"debug_normal"].get());
		normalQuadSO->SetMesh(mMeshes[L"Quad"].get());
		mSceneObjectLayer[(int)RenderLayer::Debug].push_back(normalQuadSO.get());
		mSceneObjects[normalQuadSO->UniqueName] = std::move(normalQuadSO);

		std::unique_ptr<GRiSceneObject> worldPosQuadSO(pRendererFactory->CreateSceneObject());
		worldPosQuadSO->UniqueName = L"WorldPosQuad";
		worldPosQuadSO->SetScale(.2f, .2f, .2f);
		worldPosQuadSO->SetLocation(.4f, 0.f, 0.f);
		worldPosQuadSO->UpdateTransform();
		worldPosQuadSO->ResetPrevTransform();
		worldPosQuadSO->SetTexTransform(GGiFloat4x4::Identity());
		worldPosQuadSO->SetObjIndex(mSceneObjectIndex++);
		worldPosQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"debug_velocity"].get());
		worldPosQuadSO->SetMesh(mMeshes[L"Quad"].get());
		mSceneObjectLayer[(int)RenderLayer::Debug].push_back(worldPosQuadSO.get());
		mSceneObjects[worldPosQuadSO->UniqueName] = std::move(worldPosQuadSO);

		std::unique_ptr<GRiSceneObject> roughnessQuadSO(pRendererFactory->CreateSceneObject());
		roughnessQuadSO->UniqueName = L"RoughnessQuad";
		roughnessQuadSO->SetScale(.2f, .2f, .2f);
		roughnessQuadSO->SetLocation(.6f, 0.f, 0.f);
		roughnessQuadSO->UpdateTransform();
		roughnessQuadSO->ResetPrevTransform();
		roughnessQuadSO->SetTexTransform(GGiFloat4x4::Identity());
		roughnessQuadSO->SetObjIndex(mSceneObjectIndex++);
		roughnessQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"debug_roughness"].get());
		roughnessQuadSO->SetMesh(mMeshes[L"Quad"].get());
		mSceneObjectLayer[(int)RenderLayer::Debug].push_back(roughnessQuadSO.get());
		mSceneObjects[roughnessQuadSO->UniqueName] = std::move(roughnessQuadSO);

		std::unique_ptr<GRiSceneObject> metallicQuadSO(pRendererFactory->CreateSceneObject());
		metallicQuadSO->UniqueName = L"MetallicQuad";
		metallicQuadSO->SetScale(.2f, .2f, .2f);
		metallicQuadSO->SetLocation(.8f, 0.f, 0.f);
		metallicQuadSO->UpdateTransform();
		metallicQuadSO->ResetPrevTransform();
		metallicQuadSO->SetTexTransform(GGiFloat4x4::Identity());
		metallicQuadSO->SetObjIndex(mSceneObjectIndex++);
		metallicQuadSO->SetOverrideMaterial(L"Quad", mMaterials[L"debug_metallic"].get());
		metallicQuadSO->SetMesh(mMeshes[L"Quad"].get());
		mSceneObjectLayer[(int)RenderLayer::Debug].push_back(metallicQuadSO.get());
		mSceneObjects[metallicQuadSO->UniqueName] = std::move(metallicQuadSO);
	}

	// Modified by Ssi
	// 从文件中导入一个透明对象
	int transparentObjFlag = 0;

	// 从文件中导入场景对象
	// Load scene objects from file.
	{
		for (auto info : mProject->mSceneObjectInfo) // 读取xml文件中物体的信息，info为xml定义的场景物体信息
		{
			std::unique_ptr<GRiSceneObject> newSO(pRendererFactory->CreateSceneObject()); // 创建物体
			newSO->UniqueName = info.UniqueName;			 // 设置物体名称
			newSO->SetTexTransform(GGiFloat4x4::Identity()); // 初始化为单位矩阵
			newSO->SetObjIndex(mSceneObjectIndex++);		 // 设置物体索引

			/*
			if (mMaterials.find(info.MaterialUniqueName) != mMaterials.end())
			{
				newSO->SetMaterial(mMaterials[info.MaterialUniqueName].get());
			}
			else
			{
				newSO->SetMaterial(mMaterials[L"Default"].get());
			}
			*/

			// 网格信息赋值（仅包含一个网格）
			if (mMeshes.find(info.MeshUniqueName) != mMeshes.end()) // 从网格资源中找到物体的网格
			{
				newSO->SetMesh(mMeshes[info.MeshUniqueName].get()); // 为物体设置网格
			}
			else
			{
				newSO->SetMesh(mMeshes[L"Sphere"].get());			// 未找到，设置为默认的球体
			}

			// 材质信息赋值
			for (auto it = info.OverrideMaterialUniqueName.begin(); it != info.OverrideMaterialUniqueName.end(); it++)
			{
				// 在资源文件中查找xml文件中定义的对应的材质
				if (mMaterials.find((*it).str2) != mMaterials.end()) // 找到xml内容中对应的材质信息
				{   // 如果在资源文件中找到了物体上要附加的材质，需要再做进一步验证
					// 为什么要验证？物体上附加的某个材质，需要和物体子网格中附加的材质一致才行，
					// 也就是说建立了某附加材质与物体子网格之间固定的关系
					bool bFound = false;

					for (auto& submesh : newSO->GetMesh()->Submeshes)
					{
						if (submesh.first == (*it).str1)
						{
							bFound = true;
							break;
						}
					}

					if (bFound)
						newSO->SetOverrideMaterial((*it).str1, mMaterials[(*it).str2].get()); // 子网格名-材质名
				}
			}

			// 变换信息赋值
			newSO->SetLocation(info.Location[0], info.Location[1], info.Location[2]);
			newSO->SetRotation(info.Rotation[0], info.Rotation[1], info.Rotation[2]);
			newSO->SetScale(info.Scale[0], info.Scale[1], info.Scale[2]);
			newSO->UpdateTransform();
			newSO->ResetPrevTransform();
			// mSceneObjectLayer[(int)RenderLayer::Deferred].push_back(newSO.get());

			// Modified by Ssi: 将透明对象载入Transparent RenderLayer中
			if (transparentObjFlag == 0)
			{
				mSceneObjectLayer[(int)RenderLayer::Transparent].push_back(newSO.get());
				transparentObjFlag = 1;
			}
			else
			{
				mSceneObjectLayer[(int)RenderLayer::Deferred].push_back(newSO.get());
			}

			mSceneObjects[newSO->UniqueName] = std::move(newSO); // 将新建物体存入场景对象中统一管理
		}

		// Modified by Ssi: 将Deferred和Transparent对象存到mSceneObjectLayer_Deferred_Transparent中统一操控
		mSceneObjectLayer_Deferred_Transparent.insert(mSceneObjectLayer_Deferred_Transparent.end(), 
			mSceneObjectLayer[(int)RenderLayer::Deferred].begin(), mSceneObjectLayer[(int)RenderLayer::Deferred].end());
		mSceneObjectLayer_Deferred_Transparent.insert(mSceneObjectLayer_Deferred_Transparent.end(),
			mSceneObjectLayer[(int)RenderLayer::Transparent].begin(), mSceneObjectLayer[(int)RenderLayer::Transparent].end());
	}

	// Load test objects.

	/*
	int sizeX = 32, sizeY = 32;

	for (int x = -sizeX / 2; x < sizeX / 2; x++)
	{
		for (int y = -sizeY / 2; y < sizeY / 2; y++)
		{
			std::unique_ptr<GRiSceneObject> testSO(pRendererFactory->CreateSceneObject());
			testSO->UniqueName = L"testObject_" + std::to_wstring(x + sizeX / 2) + L"_" + std::to_wstring(y + sizeY / 2);
			testSO->SetScale(1.0f, 1.0f, 1.0f);
			testSO->SetLocation(x * 300.0f, 0.0f, y * 300.0f);
			testSO->UpdateTransform();
			testSO->ResetPrevTransform();
			testSO->SetTexTransform(GGiFloat4x4::Identity());
			testSO->SetObjIndex(mSceneObjectIndex++);
			//testSO->SetMaterial(mMaterials[L"Default"].get());
			testSO->SetMesh(mMeshes[L"Content\\Models\\Cube.fbx"].get());
			mSceneObjectLayer[(int)RenderLayer::Deferred].push_back(testSO.get());
			mSceneObjects[testSO->UniqueName] = std::move(testSO);
		}
	}
	*/

	/*
	std::unique_ptr<GRiSceneObject> testSO(pRendererFactory->CreateSceneObject());
	testSO->UniqueName = L"testObject";
	testSO->SetScale(40.0f, 0.1f, 40.0f);
	testSO->SetLocation(0.0f, -240.f, 0.0f);
	testSO->UpdateTransform();
	testSO->ResetPrevTransform();
	testSO->SetTexTransform(pRendererFactory->CreateFloat4x4());
	testSO->SetObjIndex(mSceneObjectIndex++);
	testSO->SetMaterial(mMaterials[L"Default"].get());
	testSO->SetMesh(mMeshes[L"Box"].get());
	mSceneObjectLayer[(int)RenderLayer::Deferred].push_back(testSO.get());
	mSceneObjects[testSO->UniqueName] = std::move(testSO);
	*/
}

void GCore::LoadCameras()
{
	mCamera = std::make_unique<GRiCamera>();
	mCamera->SetRendererFactory(pRendererFactory);
	mCamera->SetPosition(0.0f, 0.0f, 0.0f);
	mCamera->SetLens(FOV_Y, mRenderer->AspectRatio(), NEAR_Z, FAR_Z);
	mCamera->InitPrevViewProj();
	mCamera->InitPrevPosition();

	// 构建Cubemap采样相机
	// Build cubemap sampler cameras.
	std::vector<float> center = { 0.0f, 0.0f, 0.0f };
	std::vector<float> worldUp = { 0.0f, 1.0f, 0.0f };

	// Look along each coordinate axis. 
	std::vector<float> targets[6] = {
		{1.0f, 0.0f, 0.0f}, // +X 
		{-1.0f, 0.0f, 0.0f}, // -X 
		{0.0f, 1.0f, 0.0f}, // +Y 
		{0.0f, -1.0f, 0.0f}, // -Y 
		{0.0f, 0.0f, 1.0f}, // +Z 
		{0.0f, 0.0f, -1.0f} // -Z 
	};

	// Use world up vector (0,1,0) for all directions except +Y/-Y.  In these cases, we 
	// are looking down +Y or -Y, so we need a different "up" vector. 
	std::vector<float> ups[6] = {
		{0.0f, 1.0f, 0.0f}, // +X 
		{0.0f, 1.0f, 0.0f}, // -X 
		{0.0f, 0.0f, -1.0f}, // +Y 
		{0.0f, 0.0f, +1.0f}, // -Y 
		{0.0f, 1.0f, 0.0f},	// +Z 
		{0.0f, 1.0f, 0.0f}	// -Z 
	};

	for (int i = 0; i < 6; ++i)
	{
		mCubemapSampleCamera[i] = std::make_unique<GRiCamera>();
		mCubemapSampleCamera[i]->SetRendererFactory(pRendererFactory);
		mCubemapSampleCamera[i]->LookAt(center, targets[i], ups[i]);
		mCubemapSampleCamera[i]->SetLens(0.5f * GGiEngineUtil::PI, 1.0f, 0.1f, 1000.0f);
		mCubemapSampleCamera[i]->UpdateViewMatrix();
		mCubemapSampleCamera[i]->InitPrevViewProj();
		mCubemapSampleCamera[i]->InitPrevPosition();
	}
}

#pragma endregion

#pragma region Update

void GCore::RecordPrevFrame(const GGiGameTimer* gt)
{
	mRenderer->IncreaseFrameCount();
	auto view = mCamera->GetView();
	auto proj = mCamera->GetProj();
	GGiFloat4x4 prevVP = view * proj;
	mCamera->SetPrevViewProj(prevVP);
	mCamera->SetPrevPosition(mCamera->GetPosition());
	// Modified by Ssi:
	for (auto so : mSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		so->SetPrevTransform(so->GetTransform());
	}

	//for (auto so : mSceneObjectLayer_Deferred_Transparent)
	//{
	//	so->SetPrevTransform(so->GetTransform());
	//}
}

void GCore::UpdateGui(const GGiGameTimer* gt)
{
	mImgui->BeginFrame();
	ImGuizmo::Enable((mSelectedSceneObject != nullptr));
	float view[16];
	float proj[16];
	float objLoc[3];
	float objRot[3];
	float objScale[3];
	std::vector<float> prevLoc;
	std::vector<float> prevRot;
	std::vector<float> prevScale;
	bool bSelectionNotNull = (mSelectedSceneObject != nullptr);
	if (bSelectionNotNull)
	{
		for (auto i = 0u; i < 4; i++)
		{
			for (auto j = 0u; j < 4; j++)
			{
				view[i * 4 + j] = mCamera->GetView().GetElement(i, j);
				proj[i * 4 + j] = mCamera->GetProj().GetElement(i, j);
				prevLoc = mSelectedSceneObject->GetLocation();
				objLoc[0] = prevLoc[0];
				objLoc[1] = prevLoc[1];
				objLoc[2] = prevLoc[2];
				prevRot = mSelectedSceneObject->GetRotation();
				objRot[0] = prevRot[0];
				objRot[1] = prevRot[1];
				objRot[2] = prevRot[2];
				prevScale = mSelectedSceneObject->GetScale();
				objScale[0] = prevScale[0];
				objScale[1] = prevScale[1];
				objScale[2] = prevScale[2];
			}
		}
	}

	// Modified by Ssi: To add alpha adjusting support.
	mImgui->SetGUIContent(
		bSelectionNotNull, 
		view,
		proj,
		objLoc,
		objRot,
		objScale,
		mCameraSpeed,
		GGiCpuProfiler::GetInstance().GetProfiles(),
		mRenderer->GetGpuProfiles(),
		mRenderer->GetClientWidth(),
		mRenderer->GetClientHeight()
	);

	bool bDirty = bSelectionNotNull && (objLoc[0] != prevLoc[0] || objLoc[1] != prevLoc[1] || objLoc[2] != prevLoc[2] ||
		objRot[0] != prevRot[0] || objRot[1] != prevRot[1] || objRot[2] != prevRot[2] ||
		objScale[0] != prevScale[0] || objScale[1] != prevScale[1] || objScale[2] != prevScale[2]
		);
	if (bDirty)
	{
		mSelectedSceneObject->SetLocation(objLoc[0], objLoc[1], objLoc[2]);
		mSelectedSceneObject->SetRotation(objRot[0], objRot[1], objRot[2]);
		mSelectedSceneObject->SetScale(objScale[0], objScale[1], objScale[2]);
		mGuiCallback->RefreshSceneObjectTransformCallback();
	}
}

#pragma endregion

#pragma region Runtime Activity

void GCore::Pick(int sx, int sy)
{
	mSelectedSceneObject = mRenderer->SelectSceneObject(sx, sy);
	if (mSelectedSceneObject != nullptr)
	{
		//call c# select function.
		mGuiCallback->SelectSceneObjectCallback(mSelectedSceneObject->UniqueName.c_str());
	}
}

#pragma endregion

#pragma region Util

// 获取指定文件夹relPath中指定格式的所有文件的相对路径
std::vector<std::wstring> GCore::GetAllFilesInFolder(std::wstring relPath, bool bCheckFormat, std::vector<std::wstring> format)
{
	std::vector<std::wstring> files;
	intptr_t hFile = 0;
	struct _wfinddata_t fileinfo; // 存放文件信息
	std::wstring fullPath = WorkDirectory + relPath; // 获取指定文件夹的完整目录
	//relPath = WorkDirectory + relPath;
	std::wstring p;

	// 查找目录中的文件，返回的文件信息存入fileinfo；查找成功将会返回用于_findnext和_findclse函数的值
	// _findfirst(目标文件说明（可包含通配符），函数将会填入的文件/目录信息) assign：字符串赋值
	hFile = _wfindfirst(p.assign(fullPath).append(L"\\*").c_str(), &fileinfo); 
	if (hFile != -1)
	{
		do
		{
			if ((fileinfo.attrib &  _A_SUBDIR)) // 文件是子目录
			{
				// 排除两个目录：当前目录.以及上级目录..
				if (wcscmp(fileinfo.name, L".") != 0 && wcscmp(fileinfo.name, L"..") != 0) // wcscmp 字符串比大小；若相等，返回零
				{
					// 递归获取子目录中的文件
					std::vector<std::wstring> folderFiles = std::move(GetAllFilesInFolder(p.assign(relPath).append(L"\\").append(fileinfo.name), bCheckFormat, format));
					// 在指定位置files.end()插入区间[begin, end)的所有元素
					files.insert(files.end(), folderFiles.begin(), folderFiles.end());
				}
			}
			else // 不是目录是文件
			{
				if (bCheckFormat) // 检查文件后缀
				{
					bool isOfFormat = false;
					std::wstring sFileName(fileinfo.name);
					std::wstring lFileName = sFileName; // 文件路径名
					// 字符串转小写
					// transform: 在[begin, end)中应用给定的操作tolower，并将操作的结果存放到另一范围内lFileName.begin
					std::transform(lFileName.begin(), lFileName.end(), lFileName.begin(), ::tolower);
					for (auto f : format) // 对于允许的每个文件类型
					{
						// find: 返回查找子串的首字符位置
						if (lFileName.find(L"." + f) == (lFileName.length() - f.length() - 1))
						{
							isOfFormat = true; // 这里排除了什么情况呢？这里检查文件类型不精准
							break;
						}
					}
					if (isOfFormat) // 将找到的文件以指定目录\\文件名的形式存到files中
						files.push_back(p.assign(relPath).append(L"\\").append(fileinfo.name)); 
				}
				else // 不检查文件后缀
					files.push_back(p.assign(relPath).append(L"\\").append(fileinfo.name));
			}
 		} while (_wfindnext(hFile, &fileinfo) == 0); // 根据hFile搜索句柄搜索，查找结果填入文件/目录信息fileinfo

		_findclose(hFile);
	}
	return files;
}

std::vector<std::wstring> GCore::GetAllFilesUnderFolder(std::wstring relPath, bool bCheckFormat, std::vector<std::wstring> format)
{
	std::vector<std::wstring> files;
	intptr_t hFile = 0;
	struct _wfinddata_t fileinfo;
	std::wstring fullPath = WorkDirectory + relPath;
	//relPath = WorkDirectory + relPath;
	std::wstring p;
	if ((hFile = _wfindfirst(p.assign(fullPath).append(L"\\*").c_str(), &fileinfo)) != -1)
	{
		do
		{
			if (!(fileinfo.attrib &  _A_SUBDIR))
			{
				if (bCheckFormat)
				{
					bool isOfFormat = false;
					std::wstring sFileName(fileinfo.name);
					std::wstring lFileName = sFileName;
					std::transform(lFileName.begin(), lFileName.end(), lFileName.begin(), ::tolower);
					for (auto f : format)
					{
						if (lFileName.find(L"." + f) == (lFileName.length() - f.length() - 1))
						{
							isOfFormat = true;
							break;
						}
					}
					if (isOfFormat)
						files.push_back(p.assign(relPath).append(L"\\").append(fileinfo.name));
				}
				else
					files.push_back(p.assign(relPath).append(L"\\").append(fileinfo.name));
			}
		} while (_wfindnext(hFile, &fileinfo) == 0);

		_findclose(hFile);
	}
	return files;
}

#pragma endregion

#pragma region Export

int GCore::GetSceneObjectNum()
{
	// Modified by Ssi: 
	// return (int)(mSceneObjectLayer[(int)RenderLayer::Deferred].size());
	return (int)(mSceneObjectLayer_Deferred_Transparent.size());
}

const wchar_t* GCore::GetSceneObjectName(int index)
{
	// Modified by Ssi:
	// return mSceneObjectLayer[(int)RenderLayer::Deferred][index]->UniqueName.c_str();

	return mSceneObjectLayer_Deferred_Transparent[index]->UniqueName.c_str();
}

void GCore::GetSceneObjectTransform(wchar_t* objName, float* trans)
{
	std::wstring sObjectName(objName);

	std::vector<float> loc = mSceneObjects[sObjectName]->GetLocation();
	std::vector<float> rot = mSceneObjects[sObjectName]->GetRotation();
	std::vector<float> scale = mSceneObjects[sObjectName]->GetScale();

	trans[0] = loc[0];
	trans[1] = loc[1];
	trans[2] = loc[2];
	trans[3] = rot[0];
	trans[4] = rot[1];
	trans[5] = rot[2];
	trans[6] = scale[0];
	trans[7] = scale[1];
	trans[8] = scale[2];
}

void GCore::SetSceneObjectTransform(wchar_t* objName, float* trans)
{
	std::wstring sObjectName(objName);

	mSceneObjects[sObjectName]->SetLocation(trans[0], trans[1], trans[2]);
	mSceneObjects[sObjectName]->SetRotation(trans[3], trans[4], trans[5]);
	mSceneObjects[sObjectName]->SetScale(trans[6], trans[7], trans[8]);
}

bool GCore::GetTextureSrgb(wchar_t* txtName)
{
	std::wstring textureName(txtName);
	return mTextures[textureName]->bSrgb;
}

void GCore::SetTextureSrgb(wchar_t* txtName, bool bSrgb)
{
	std::wstring textureName(txtName);
	mTextures[textureName]->bSrgb = bSrgb;
	std::unique_ptr<GRiTextureLoader> textureLoader(pRendererFactory->CreateTextureLoader());
	GRiTexture* tex = textureLoader->LoadTexture(WorkDirectory, textureName, bSrgb);
	tex->texIndex = mTextures[textureName]->texIndex;
	mTextures[textureName].reset(tex);
	mRenderer->RegisterTexture(mTextures[textureName].get());
}

// 设置工作目录，获取并设置引擎的工作目录
void GCore::SetWorkDirectory(wchar_t* dir)
{
	std::wstring path(dir);
	WorkDirectory = path;

	TCHAR exeFullPath[MAX_PATH];
	memset(exeFullPath, 0, MAX_PATH); // 内存初始化：将exeFullPath后面的MAX_PATH个字节用0替换并返回exeFullPath

	// 获取当前进程已加载模块文件的完整路径
	// 参数说明：模块句柄：如果为NULL，该函数返回应用程序的全路径；字符串缓冲区；装载到缓冲区的最大字符数量
	GetModuleFileName(NULL, exeFullPath, MAX_PATH); 
	WCHAR *p = wcsrchr(exeFullPath, '\\'); // 从一个字符串中寻找某个字符最后出现的位置
	*p = 0x00;

	EngineDirectory = std::wstring(exeFullPath); // 设置引擎工作目录
	EngineDirectory += L"\\";
}

// 设置项目名字
void GCore::SetProjectName(wchar_t* projName)
{
	std::wstring pjName(projName);
	ProjectName = pjName;
}

void GCore::SaveProject()
{
	for (auto it = mMaterialFiles.begin(); it != mMaterialFiles.end(); it++)
	{
		(*it).second->SaveMaterial(WorkDirectory);
	}

	// Modified by Ssi:
	// mProject->SaveProject(WorkDirectory + ProjectName + L".gproj", mSkyCubemapUniqueName, mTextures, mSceneObjectLayer[(int)RenderLayer::Deferred], mMeshes);
	mProject->SaveProject(WorkDirectory + ProjectName + L".gproj", mSkyCubemapUniqueName, mTextures, mSceneObjectLayer_Deferred_Transparent, mMeshes);
}

void GCore::LoadProject()
{
	mProject->LoadProject(WorkDirectory + ProjectName + L".gproj"); // 根据OpenProject的结果加载项目
}

void GCore::CreateMaterial(wchar_t* cUniqueName)
{
	std::wstring UniqueName(cUniqueName);
	auto newMat = std::make_unique<GRiMaterial>(*pRendererFactory->CreateMaterial());
	newMat->UniqueName = UniqueName;
	newMat->Name = GGiEngineUtil::GetFileName(UniqueName);
	newMat->MatIndex = mMaterialIndex++;
	newMat->AddTexture(mTextures[L"Resource\\Textures\\GE_DefaultTexture_Albedo.png"].get());
	newMat->AddTexture(mTextures[L"Resource\\Textures\\GE_DefaultTexture_Normal.png"].get());
	newMat->AddTexture(mTextures[L"Resource\\Textures\\GE_DefaultTexture_Orm.png"].get());
	mMaterials[UniqueName] = std::move(newMat);
	auto matFile = std::make_unique<GMaterial>(mMaterials[UniqueName].get());
	matFile->SaveMaterial(WorkDirectory);
	mMaterialFiles[UniqueName] = std::move(matFile);
	mRenderer->SyncMaterials(mMaterials);
}

void GCore::GetMaterialScale(wchar_t* matUniqueName, float* scale)
{
	std::wstring UniqueName(matUniqueName);
	if (mMaterials.find(UniqueName) == mMaterials.end())
	{
		scale[0] = -1.0f;
		scale[1] = -1.0f;
		return;
	}
	scale[0] = mMaterials[UniqueName]->GetScaleX();
	scale[1] = mMaterials[UniqueName]->GetScaleY();
}

void GCore::SetMaterialScale(wchar_t* matUniqueName, float* scale)
{
	std::wstring UniqueName(matUniqueName);
	if (mMaterials.find(UniqueName) == mMaterials.end())
	{
		scale[0] = -1.0f;
		scale[1] = -1.0f;
		return;
	}
	mMaterials[UniqueName]->SetScale(scale[0], scale[1]);
}

const wchar_t* GCore::GetMaterialTextureUniqueName(wchar_t* matUniqueName, int index)
{
	std::wstring UniqueName(matUniqueName);
	if (mMaterials.find(UniqueName) == mMaterials.end())
	{
		return L"none";
	}
	std::wstring* txtName = &(mMaterials[UniqueName]->GetTextureUniqueNameByIndex(index));
	return txtName->c_str();
}

bool GCore::SetMaterialTexture(wchar_t* matUniqueName, int index, wchar_t* texUniqueName)
{
	std::wstring materialUniqueName(matUniqueName);
	std::wstring textureUniqueName(texUniqueName);
	if (mMaterials.find(materialUniqueName) == mMaterials.end())
	{
		return false;
	}
	if (mTextures.find(textureUniqueName) == mTextures.end())
	{
		return false;
	}
	mMaterials[materialUniqueName]->SetTextureByIndex(index, mTextures[texUniqueName].get());
	return true;
}

void GCore::SetMaterialTextureToDefaultValue(wchar_t* matUniqueName, int index)
{
	std::wstring UniqueName(matUniqueName);
	if (mMaterials.find(UniqueName) == mMaterials.end())
	{
		return;
	}
	std::wstring texName;
	if (index == 0)
		texName = L"Resource\\Textures\\GE_DefaultTexture_Albedo.png";
	else if (index == 1)
		texName = L"Resource\\Textures\\GE_DefaultTexture_Normal.png";
	else if (index == 2)
		texName = L"Resource\\Textures\\GE_DefaultTexture_Orm.png";
	else
		ThrowGGiException("Texture index overflow.");
	mMaterials[UniqueName]->SetTextureByIndex(index, mTextures[texName].get());
}

void GCore::RenameMaterial(wchar_t* oldUniqueName, wchar_t* newUniqueName)
{
	std::wstring oldUniqueNameStr(oldUniqueName);
	std::wstring newUniqueNameStr(newUniqueName);
	if (mMaterials.find(oldUniqueNameStr) == mMaterials.end())
	{
		return;
	}
	if (mMaterials.find(newUniqueNameStr) != mMaterials.end())
	{
		return;
	}
	std::unique_ptr<GRiMaterial> toMove = std::move(mMaterials[oldUniqueNameStr]);
	toMove->UniqueName = newUniqueNameStr;
	toMove->Name = GGiEngineUtil::GetFileName(newUniqueNameStr);
	mMaterials[newUniqueNameStr] = std::move(toMove);
	mMaterials.erase(oldUniqueNameStr);
	mRenderer->SyncMaterials(mMaterials);
}

void GCore::SetSceneObjectMesh(wchar_t* sceneObjectName, wchar_t* meshUniqueName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	std::wstring MeshNameStr(meshUniqueName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return;
	}
	if (mMeshes.find(MeshNameStr) == mMeshes.end())
	{
		return;
	}
	mSceneObjects[SceneObjectNameStr]->SetMesh(mMeshes[MeshNameStr].get());
	mSceneObjects[SceneObjectNameStr]->ClearOverrideMaterials();
}

/*
void GCore::SetSceneObjectMaterial(wchar_t* sceneObjectName, wchar_t* matUniqueName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	std::wstring MatNameStr(matUniqueName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return;
	}
	if (mMaterials.find(MatNameStr) == mMaterials.end())
	{
		return;
	}
	//mSceneObjects[SceneObjectNameStr]->SetMaterial(mMaterials[MatNameStr].get());
}
*/

const wchar_t* GCore::GetSceneObjectMeshName(wchar_t* sceneObjectName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return L"None";
	}
	return mSceneObjects[SceneObjectNameStr]->GetMesh()->UniqueName.c_str();
}

/*
const wchar_t* GCore::GetSceneObjectMaterialName(wchar_t* sceneObjectName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return L"None";
	}
	//return mSceneObjects[SceneObjectNameStr]->GetMaterial()->UniqueName.c_str();
	return L"None";
}
*/

bool GCore::SceneObjectExists(wchar_t* sceneObjectName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	return (mSceneObjects.find(SceneObjectNameStr) != mSceneObjects.end());
}

void GCore::CreateSceneObject(wchar_t* sceneObjectName, wchar_t* meshUniqueName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	std::wstring MeshUniqueNameStr(meshUniqueName);
	if (mSceneObjects.find(SceneObjectNameStr) != mSceneObjects.end())
	{
		return;
	}
	if (mMeshes.find(MeshUniqueNameStr) == mMeshes.end())
	{
		return;
	}
	std::unique_ptr<GRiSceneObject> newSceneObject(pRendererFactory->CreateSceneObject());
	newSceneObject->UniqueName = SceneObjectNameStr;
	newSceneObject->UpdateTransform();
	newSceneObject->ResetPrevTransform();
	newSceneObject->SetTexTransform(GGiFloat4x4::Identity());
	newSceneObject->SetObjIndex(mSceneObjectIndex++);
	//newSceneObject->SetMaterial(mMaterials[L"Default"].get());
	newSceneObject->SetMesh(mMeshes[MeshUniqueNameStr].get());
	mSceneObjectLayer[(int)RenderLayer::Deferred].push_back(newSceneObject.get());

	// 目前默认新建的都是不透明物体【Deferred】
	// Modified by Ssi: 保证mSceneObjectLayer_Deferred_Transparent表和mSceneObjectLayer Deferred表同步
	mSceneObjectLayer_Deferred_Transparent.push_back(newSceneObject.get());

	mSceneObjects[newSceneObject->UniqueName] = std::move(newSceneObject);
	mRenderer->SyncSceneObjects(mSceneObjects, mSceneObjectLayer);
}

// 重命名场景物体
void GCore::RenameSceneObject(wchar_t* oldName, wchar_t* newName)
{
	std::wstring OldNameStr(oldName);
	std::wstring NewNameStr(newName);
	if (mSceneObjects.find(OldNameStr) == mSceneObjects.end())
	{
		return;
	}
	if (mSceneObjects.find(NewNameStr) != mSceneObjects.end())
	{
		return;
	}
	std::unique_ptr<GRiSceneObject> toMove = std::move(mSceneObjects[OldNameStr]);
	toMove->UniqueName = NewNameStr;
	mSceneObjects[NewNameStr] = std::move(toMove);
	mSceneObjects.erase(OldNameStr);
	mRenderer->SyncSceneObjects(mSceneObjects, mSceneObjectLayer);

	// Modified by Ssi: 为mSceneObjectLayer_Deferred_Transparent表中物体重命名

}

// 删除场景物体
void GCore::DeleteSceneObject(wchar_t* sceneObjectName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return;
	}
	auto it = std::find(mSceneObjectLayer[(int)RenderLayer::Deferred].begin(),
		mSceneObjectLayer[(int)RenderLayer::Deferred].end(),
		mSceneObjects[SceneObjectNameStr].get());
	if (it != mSceneObjectLayer[(int)RenderLayer::Deferred].end())
	{
		mSceneObjectLayer[(int)RenderLayer::Deferred].erase(it);
	}
	mSceneObjects.erase(SceneObjectNameStr);
	mRenderer->SyncSceneObjects(mSceneObjects, mSceneObjectLayer);

	// Modified by Ssi: 删除mSceneObjectLayer_Deferred_Transparent表中物体
	//auto it_DTObject = std::find(mSceneObjectLayer_Deferred_Transparent.begin(),
	//	mSceneObjectLayer_Deferred_Transparent.end(), mSceneObjects[SceneObjectNameStr].get());
	//if (it_DTObject != mSceneObjectLayer_Deferred_Transparent.end())
	//{
	//	mSceneObjectLayer_Deferred_Transparent.erase(it_DTObject);
	//}
}

const wchar_t* GCore::GetSkyCubemapUniqueName()
{
	return mSkyCubemapUniqueName.c_str();
}

const wchar_t* GCore::GetDefaultSkyCubemapUniqueName()
{
	return L"Resource\\Textures\\GE_Default_Cubemap.dds";
}

void GCore::SetSkyCubemapUniqueName(wchar_t* newName)
{
	std::wstring newNameStr(newName);
	mSkyCubemapUniqueName = newNameStr;
}

bool GCore::SkyCubemapNameAvailable(wchar_t* cubemapName)
{
	std::wstring cubemapNameStr(cubemapName);
	std::wstring defaultNameStr(GetDefaultSkyCubemapUniqueName());
	if (cubemapNameStr == defaultNameStr)
		return true;
	if (mTextures.find(cubemapNameStr) == mTextures.end())
		return false;
	else
		return true;
}

void GCore::SetSelectSceneObjectCallback(VoidWstringFuncPointerType callback)
{
	mGuiCallback->SetSelectSceneObjectCallback(callback);
}

void GCore::SelectSceneObject(wchar_t* sceneObjectName)
{
	std::wstring SceneObjectNameStr(sceneObjectName);
	if (mSceneObjects.find(SceneObjectNameStr) == mSceneObjects.end())
	{
		return;
	}
	mSelectedSceneObject = mSceneObjects[SceneObjectNameStr].get();
}

void GCore::SetRefreshSceneObjectTransformCallback(VoidFuncPointerType pRefreshSceneObjectTransformCallback)
{
	mGuiCallback->SetRefreshSceneObjectTransformCallback(pRefreshSceneObjectTransformCallback);
}

int GCore::GetMeshSubmeshCount(wchar_t* meshName)
{
	std::wstring strMeshName(meshName);
	auto it = mMeshes.find(strMeshName);
	if (it != mMeshes.end())
	{
		return (int)((*it).second->Submeshes.size());
	}
	else
		return 0;
}

wchar_t** GCore::GetMeshSubmeshNames(wchar_t* meshName)
{
	std::wstring strMeshName(meshName);
	auto it = mMeshes.find(strMeshName);
	if (it == mMeshes.end())
		return nullptr;
	auto submeshCount = (*it).second->Submeshes.size();
	wchar_t** ret = new wchar_t*[submeshCount];

	int i = 0;
	for (auto submesh : (*it).second->Submeshes)
	{
		ret[i] = new wchar_t[256];
		std::wcsncpy(ret[i], submesh.second.Name.c_str(), 128);
		i++;
	}

	return ret;
}

const wchar_t* GCore::GetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName)
{
	std::wstring strMeshName(meshName);
	auto it = mMeshes.find(strMeshName);
	if (it == mMeshes.end())
		return nullptr;

	std::wstring strSubmeshName(submeshName);
	auto itsub = mMeshes[strMeshName]->Submeshes.find(strSubmeshName);
	if (itsub == mMeshes[strMeshName]->Submeshes.end())
		return nullptr;

	return mMeshes[strMeshName]->Submeshes[strSubmeshName].GetMaterial()->UniqueName.c_str();
}

void GCore::SetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName, wchar_t* materialName)
{
	std::wstring strMeshName(meshName);
	auto it = mMeshes.find(strMeshName);
	if (it == mMeshes.end())
		return;

	std::wstring strSubmeshName(submeshName);
	auto itsub = mMeshes[strMeshName]->Submeshes.find(strSubmeshName);
	if (itsub == mMeshes[strMeshName]->Submeshes.end())
		return;

	std::wstring strMatName(materialName);
	auto itmat = mMaterials.find(strMatName);
	if (itmat == mMaterials.end())
		return;

	mMeshes[strMeshName]->Submeshes[strSubmeshName].SetMaterial(mMaterials[strMatName].get());
}

const wchar_t* GCore::GetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName)
{
	std::wstring strSceneObjectName(soName);
	auto it = mSceneObjects.find(strSceneObjectName);
	if (it == mSceneObjects.end())
		return nullptr;

	std::wstring strSubmeshName(submeshName);
	auto mat = mSceneObjects[strSceneObjectName]->GetOverrideMaterial(strSubmeshName);
	if (mat == nullptr)
		return L"None";
	else
		return mat->UniqueName.c_str();
}

void GCore::SetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName, wchar_t* materialName)
{
	std::wstring strSceneObjectName(soName);
	auto it = mSceneObjects.find(strSceneObjectName);
	if (it == mSceneObjects.end())
		return;

	std::wstring strSubmeshName(submeshName);

	std::wstring strMatName(materialName);
	auto itmat = mMaterials.find(strMatName);
	if (itmat == mMaterials.end() && strMatName != L"None")
		return;

	if (strMatName == L"None")
		mSceneObjects[strSceneObjectName]->SetOverrideMaterial(strSubmeshName, nullptr);
	else
		mSceneObjects[strSceneObjectName]->SetOverrideMaterial(strSubmeshName, (*itmat).second.get());
}

void GCore::SetTestValue(int index, float value)
{
	mRenderer->TestValue[index] = value;
}

void GCore::SetTestBool(bool value)
{
	mRenderer->TestBool = value;
}

#pragma endregion


