﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;
using System.IO;
using System.Collections.ObjectModel;
using GEditor.Model.FileBrowser;
using System.Runtime.InteropServices;

namespace GEditor.View
{
    /// <summary>
    /// Interaction logic for FileBrowser.xaml
    /// </summary>
    public partial class FileBrowser : UserControl
    {
        private string workDirectory = string.Empty;
        private string currentDirectory = string.Empty;
        Dictionary<string, BitmapImage> treeViewIcons = new Dictionary<string, BitmapImage>();
        Dictionary<string, BitmapImage> listBoxIcons = new Dictionary<string, BitmapImage>();
        ObservableCollection<BrowserListBoxItemModel> listBoxData = new ObservableCollection<BrowserListBoxItemModel>();
        //List<BrowserListBoxItemModel> listBoxData = new List<BrowserListBoxItemModel>();

        MainWindow mainWindow;

        public FileBrowser()
        {
            InitializeComponent();

            LoadImages();

            //OnBrowserLoaded(); 
            browserListBox.ItemsSource = listBoxData;
        }

        void LoadImages()
        {
            BitmapImage img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_tv_folder.png", UriKind.Absolute);
            img.EndInit();
            treeViewIcons.Add("folder", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_tv_document.png", UriKind.Absolute);
            img.EndInit();
            treeViewIcons.Add("document", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_tv_image.png", UriKind.Absolute);
            img.EndInit();
            treeViewIcons.Add("image", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_tv_material.png", UriKind.Absolute);
            img.EndInit();
            treeViewIcons.Add("material", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_tv_mesh.png", UriKind.Absolute);
            img.EndInit();
            treeViewIcons.Add("mesh", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_lb_folder.png", UriKind.Absolute);
            img.EndInit();
            listBoxIcons.Add("folder", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_lb_document.png", UriKind.Absolute);
            img.EndInit();
            listBoxIcons.Add("document", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_lb_image.png", UriKind.Absolute);
            img.EndInit();
            listBoxIcons.Add("image", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_lb_material.png", UriKind.Absolute);
            img.EndInit();
            listBoxIcons.Add("material", img);
            img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.CreateOptions = BitmapCreateOptions.None;
            img.UriSource = new Uri(AppDomain.CurrentDomain.BaseDirectory + "..\\GEditor\\Resource\\Image\\icon_lb_mesh.png", UriKind.Absolute);
            img.EndInit();
            listBoxIcons.Add("mesh", img);
        }

        void item_Selected(object sender, RoutedEventArgs e)
        {
            TreeViewItem item = e.OriginalSource as TreeViewItem;
            BrowserTreeViewItemModel model = item.DataContext as BrowserTreeViewItemModel;
            if(model.IsFolder)
            {
                GetFiles(model.Path);
            }
        }

        private BrowserListBoxItemModel GetListBoxItemByName(string itemName)
        {
            foreach(object item in browserListBox.Items)
            {
                BrowserListBoxItemModel model = item as BrowserListBoxItemModel;
                if (model.FileName == itemName)
                    return model;
            }
            return null;
        }

        public void RefreshListBox()
        {
            GetFiles(currentDirectory);
        }

        private void GetFiles(string FilePath)
         {
            listBoxData.Clear();
            //browserListBox.ItemsSource = null;
            currentDirectory = FilePath;

            foreach (var path in Directory.GetDirectories(FilePath))
            {

                // if @path is a folder
                if (Directory.Exists(@path))
                {
                    DirectoryInfo dir = new DirectoryInfo(path);

                    // if this directory is not readonly or hidden
                    if (FileAttributes.Directory == dir.Attributes || dir.Attributes.ToString().Equals("ReadOnly, Directory"))
                    {
                        BrowserListBoxItemModel model = new BrowserListBoxItemModel();
                        model.Icon = listBoxIcons["folder"];
                        model.FileName = dir.Name;
                        model.FileType = "Folder";
                        model.FilePath = @path;
                        listBoxData.Add(model);
                    }
                }

            }

            foreach (var path in Directory.GetFiles(FilePath))
            {
                if (File.Exists(@path))
                {

                    FileInfo file = new FileInfo(@path);
                    
                    {
                        BrowserListBoxItemModel model = new BrowserListBoxItemModel();
                        if (file.Extension.ToLower().Equals(".dds") || file.Extension.ToLower().Equals(".png") || file.Extension.ToLower().Equals(".jpg") || file.Extension.ToLower().Equals(".tga"))
                            model.Icon = listBoxIcons["image"];
                        else if (file.Extension.ToLower().Equals(".fbx"))
                            model.Icon = listBoxIcons["mesh"];
                        else if (file.Extension.ToLower().Equals(".gmat"))
                            model.Icon = listBoxIcons["material"];
                        else
                            model.Icon = listBoxIcons["document"];
                        model.FileName = file.Name;
                        model.FileType = file.Extension;
                        model.FilePath = @path;
                        
                        listBoxData.Add(model);
                    }

                }
            }

            //if (listBoxData.Count > 0)
            {
                //browserListBox.ItemsSource = listBoxData;
            }
        }

        void folder_Expanded(object sender, RoutedEventArgs e)
        {
            TreeViewItem item = e.OriginalSource as TreeViewItem;
            BrowserTreeViewItemModel model = item.DataContext as BrowserTreeViewItemModel;
            foreach(BrowserTreeViewItemModel child in model.Children)
            {
                if(child.IsFolder)
                {
                    child.Children = GetChildrenByPath(child.Path);
                }
            }
        }
        
        public void LoadBrowser()
        {
            String ContentFolder = workDirectory + @"Content\"; // Content文件路径
            if (ContentFolder == null)
            {
                MessageBox.Show("ContentFolder is null");
                return;
            }
            browserTreeView.ItemsSource = null;
            //browserTreeView.Items.Clear();

            List<BrowserTreeViewItemModel> root = new List<BrowserTreeViewItemModel>();
            
            BrowserTreeViewItemModel content = new BrowserTreeViewItemModel();
            content.Text = "Content";
            content.Icon = treeViewIcons["folder"];
            //content.Path = AppDomain.CurrentDomain.BaseDirectory + @"Content\";
            content.Path = workDirectory + @"Content\";
            content.IsFolder = true;
            content.Children = GetChildrenByPath(content.Path);
            
            root.Add(content);

            browserTreeView.ItemsSource = root;
        }

        private List<BrowserTreeViewItemModel> GetChildrenByPath(string FilePath)
        {
            List<BrowserTreeViewItemModel> children = new List<BrowserTreeViewItemModel>();
            
            foreach (var path in Directory.GetDirectories(FilePath)) // 返回指定目录中子目录的路径，return String[]
            {

                // if @path is a folder
                if (Directory.Exists(@path)) // 判断指定的文件夹是否存在
                {
                    DirectoryInfo dir = new DirectoryInfo(path); // 用于描述一个文件夹对象

                    // if this directory is not readonly or hidden
                    // FileAttributes.Direcory表示文件是一个目录
                    if (FileAttributes.Directory == dir.Attributes || dir.Attributes.ToString().Equals("ReadOnly, Directory")) // 该文件为目录且只读
                    {
                        BrowserTreeViewItemModel model = new BrowserTreeViewItemModel();
                        model.Text = dir.Name;
                        model.Icon = treeViewIcons["folder"];
                        model.Path = @path;
                        model.IsFolder = true;
                        children.Add(model);
                    }
                }

            }

            foreach (var path in Directory.GetFiles(FilePath)) // 返回指定目录中子文件的路径，return String[]
            {
                // file
                if (File.Exists(@path)) // 判断指定文件是否存在
                {

                    FileInfo file = new FileInfo(@path);

                    //if (file.Extension.Equals(".fbx") || file.Extension.Equals(".png"))
                    {
                        BrowserTreeViewItemModel model = new BrowserTreeViewItemModel();
                        model.Text = file.Name;
                        if (file.Extension.ToLower().Equals(".dds") || file.Extension.ToLower().Equals(".png") || file.Extension.ToLower().Equals(".jpg") || file.Extension.ToLower().Equals(".tga"))
                            model.Icon = treeViewIcons["image"];
                        else if (file.Extension.ToLower().Equals(".fbx"))
                            model.Icon = treeViewIcons["mesh"];
                        else if (file.Extension.ToLower().Equals(".gmat"))
                            model.Icon = treeViewIcons["material"];
                        else
                            model.Icon = treeViewIcons["document"];
                        model.Path = @path;
                        model.IsFolder = false;
                        children.Add(model);
                    }

                }
            }

            return children;
        }

        // mouse double click
        private void Border_MouseDown(object sender, MouseButtonEventArgs e)
        {
            //if (e.ClickCount == 2)
            {
                BrowserListBoxItemModel model = (BrowserListBoxItemModel)browserListBox.SelectedItem;
                if (model == null)
                    return;
                if (model.FileType.Equals("Folder"))
                {
                    GetFiles(model.FilePath);
                }

                string ftype = model.FileType.ToLower();
                if (ftype == ".dds" || ftype == ".png" || ftype == ".jpg" || ftype == ".tga")
                {
                    if (model.FilePath.IndexOf(workDirectory) != -1)
                    {
                        string txtName = model.FilePath.Substring(model.FilePath.IndexOf(workDirectory) + workDirectory.Length);
                        mainWindow.GetTextureProperties(txtName);
                    }
                }
                else if (ftype == ".gmat")
                {
                    if (model.FilePath.IndexOf(workDirectory) != -1)
                    {
                        string matName = model.FilePath.Substring(workDirectory.Length);
                        mainWindow.GetMaterialPropertiesByUniqueName(matName);
                    }
                }
                else if (ftype == ".fbx")
                {
                    if (model.FilePath.IndexOf(workDirectory) != -1)
                    {
                        string meshName = model.FilePath.Substring(workDirectory.Length);
                        mainWindow.GetMeshPropertiesByUniqueName(meshName);
                    }
                }

            }

        }

        // return to last directory
        private void GoToParentFolder(object sender, MouseButtonEventArgs e)
        {
            if (currentDirectory.LastIndexOf(@"\") > 0)
            {
                currentDirectory = currentDirectory.Substring(0, currentDirectory.LastIndexOf(@"\"));
            }
            else
            {
                currentDirectory = currentDirectory = currentDirectory.Substring(0, currentDirectory.IndexOf("/") + 1);
            }
            GetFiles(currentDirectory);
        }

        public void SetMainWindow(MainWindow mwRef)
        {
            mainWindow = mwRef;
        }

        public void SetWorkDirectory(string dir)
        {
            workDirectory = dir;
        }

        public string GetWorkDirectory()
        {
            return workDirectory;
        }

        private void listBoxItem_Selected(object sender, RoutedEventArgs e)
        {
            /*
            BrowserListBoxItemModel selected = (BrowserListBoxItemModel)browserListBox.SelectedItem;
            if (selected == null)
                return;
            string ftype = selected.FileType.ToLower();
            if (ftype != ".dds" && ftype != ".png")
                return;
            if (selected.FilePath.IndexOf(workDirectory) != -1)
            {
                string txtName = selected.FilePath.Substring(selected.FilePath.IndexOf(workDirectory) + workDirectory.Length);
                mainWindow.GetTextureProperties(txtName);
            }
            */
        }

        private void CreateMaterial(object sender, RoutedEventArgs e)
        {
            string materialName = string.Empty;
            for (int i = 0; ; i++)
            {
                string tryName = "NewMaterial";
                if (i > 0)
                    tryName = tryName + "_" + Convert.ToString(i);
                if (!File.Exists(currentDirectory + @"\" + tryName + ".gmat"))
                {
                    materialName = currentDirectory + @"\" + tryName + ".gmat";
                    break;
                }
            }
            string UniqueName = materialName.Substring(workDirectory.Length);
            string fileName = System.IO.Path.GetFileName(materialName);

            IGCore.CreateMaterial(UniqueName);

            GetFiles(currentDirectory);
            browserListBox.SelectedItem = GetListBoxItemByName(fileName);
            mainWindow.GetMaterialPropertiesByUniqueName(UniqueName);
        }

        public string GetSelectedFilePath()
        {
            BrowserListBoxItemModel selected = browserListBox.SelectedItem as BrowserListBoxItemModel;
            if(selected!=null)
                return selected.FilePath;
            else
                return string.Empty;
        }

        public string GetSelectedFileUniqueName()
        {
            BrowserListBoxItemModel selected = browserListBox.SelectedItem as BrowserListBoxItemModel;
            if (selected != null)
                return selected.FilePath.Substring(workDirectory.Length);
            else
                return string.Empty;
        }

        private void BrowserListBox_CleanUpVirtualizedItem(object sender, CleanUpVirtualizedItemEventArgs e)
        {

        }
    }
}
