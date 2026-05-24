import SwiftUI

struct ImagePopupView: View {
    @State var group: Vurio_Dedup_DuplicateGroup
    @Binding var isPresented: Bool
    var initialIndex: Int = 0
    var onEjectStatusChanged: ((String, Bool) -> Void)?
    
    @State private var selectedIndex: Int = 0
    @State private var scale: CGFloat = 1.0
    @State private var rotation: Double = 0
    @State private var isEjecting: Bool = false
    @State private var currentImage: NSImage? = nil
    @State private var isLoadingImage: Bool = false
    
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Group ID: \(group.groupID)")
                    .font(.headline)
                Spacer()
                Button("Close") {
                    isPresented = false
                }
                .keyboardShortcut(.escape, modifiers: [])
            }
            .padding()
            .background(Color(NSColor.windowBackgroundColor))
            
            GeometryReader { geometry in
                ZStack {
                    Color.black.ignoresSafeArea()
                    
                    if group.files.indices.contains(selectedIndex) {
                        if let image = currentImage {
                            Image(nsImage: image)
                                .resizable()
                                .aspectRatio(contentMode: .fit)
                                .scaleEffect(scale)
                                .rotationEffect(.degrees(rotation))
                                .frame(width: geometry.size.width, height: geometry.size.height)
                                .clipped()
                                .gesture(
                                    MagnificationGesture()
                                        .onChanged { value in
                                            scale = value.magnitude
                                        }
                                )
                        } else if isLoadingImage {
                            ProgressView()
                                .controlSize(.large)
                        } else {
                            VStack {
                                Image(systemName: "photo.badge.exclamationmark")
                                    .font(.system(size: 64))
                                    .foregroundColor(.gray)
                                Text("Failed to load image")
                                    .foregroundColor(.gray)
                            }
                        }
                    }
                    
                    // Navigation Buttons
                    HStack {
                        Button(action: {
                            if selectedIndex > 0 { selectedIndex -= 1 }
                            resetTransform()
                        }) {
                            Image(systemName: "chevron.left.circle.fill")
                                .font(.system(size: 40))
                                .foregroundColor(.white.opacity(0.7))
                        }
                        .buttonStyle(PlainButtonStyle())
                        .disabled(selectedIndex <= 0)
                        
                        Spacer()
                        
                        Button(action: {
                            if selectedIndex < group.files.count - 1 { selectedIndex += 1 }
                            resetTransform()
                        }) {
                            Image(systemName: "chevron.right.circle.fill")
                                .font(.system(size: 40))
                                .foregroundColor(.white.opacity(0.7))
                        }
                        .buttonStyle(PlainButtonStyle())
                        .disabled(selectedIndex >= group.files.count - 1)
                    }
                    .padding()
                }
            }
            
            // Toolbar
            HStack {
                Button(action: { scale = max(scale / 1.5, 0.1) }) { Image(systemName: "minus.magnifyingglass") }
                Button(action: resetTransform) { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
                Button(action: { scale = min(scale * 1.5, 10.0) }) { Image(systemName: "plus.magnifyingglass") }
                
                Divider().frame(height: 20)
                
                Button(action: { rotation -= 90 }) { Image(systemName: "rotate.left") }
                Button(action: { rotation += 90 }) { Image(systemName: "rotate.right") }
                
                if group.files.indices.contains(selectedIndex) {
                    let currentFile = group.files[selectedIndex]
                    if !currentFile.isRepresentative {
                        Divider().frame(height: 20)
                        
                        if currentFile.isEjected {
                            Button(action: {
                                Task { await handleRestore() }
                            }) {
                                Image(systemName: "arrow.uturn.backward.circle")
                                    .foregroundColor(.green)
                            }
                            .disabled(isEjecting)
                            .help("Restore Image (Delete/Backspace)")
                        } else {
                            Button(action: {
                                Task { await handleEject() }
                            }) {
                                Image(systemName: "trash")
                                    .foregroundColor(.red)
                            }
                            .disabled(isEjecting)
                            .help("Eject Image (Delete/Backspace)")
                        }
                    }
                }
            }
            .padding()
            .background(Color(NSColor.windowBackgroundColor))
        }
        .frame(minWidth: 800, minHeight: 600)
        .background(
            // Hidden buttons for keyboard shortcuts
            Group {
                if group.files.indices.contains(selectedIndex) {
                    let currentFile = group.files[selectedIndex]
                    if !currentFile.isRepresentative {
                        Button("") {
                            if currentFile.isEjected {
                                Task { await handleRestore() }
                            } else {
                                Task { await handleEject() }
                            }
                        }
                        .keyboardShortcut(.delete, modifiers: [])
                        .hidden()
                        
                        Button("") {
                            if currentFile.isEjected {
                                Task { await handleRestore() }
                            } else {
                                Task { await handleEject() }
                            }
                        }
                        .keyboardShortcut(.deleteForward, modifiers: [])
                        .hidden()
                    }
                }
            }
        )
        .onAppear {
            selectedIndex = initialIndex
            Task {
                await loadImage()
            }
        }
        .onChange(of: selectedIndex) { _ in
            Task {
                await loadImage()
            }
        }
    }
    
    private func loadImage() async {
        guard group.files.indices.contains(selectedIndex) else { return }
        let file = group.files[selectedIndex]
        
        isLoadingImage = true
        currentImage = nil
        
        do {
            let image = try await ServerImageLoader.shared.loadPreview(filePath: file.filePath)
            await MainActor.run {
                self.currentImage = image
                self.isLoadingImage = false
            }
        } catch is CancellationError {
            // Task was cancelled
        } catch {
            await MainActor.run {
                self.isLoadingImage = false
            }
        }
    }
    
    private func handleEject() async {
        guard group.files.indices.contains(selectedIndex) else { return }
        let file = group.files[selectedIndex]
        guard !file.isRepresentative && !file.isEjected && !isEjecting else { return }
        
        isEjecting = true
        defer { isEjecting = false }
        
        do {
            let req = Vurio_Dedup_EjectDuplicatesRequest.with { $0.filePaths = [file.filePath] }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.ejectDuplicates(req)
                if res.success {
                    group.files[selectedIndex].isEjected = true
                    onEjectStatusChanged?(file.filePath, true)
                }
            }
        } catch {
            print("Eject error: \(error)")
        }
    }
    
    private func handleRestore() async {
        guard group.files.indices.contains(selectedIndex) else { return }
        let file = group.files[selectedIndex]
        guard !file.isRepresentative && file.isEjected && !isEjecting else { return }
        
        isEjecting = true
        defer { isEjecting = false }
        
        do {
            let req = Vurio_Dedup_UndoEjectDuplicatesRequest.with { $0.filePaths = [file.filePath] }
            if let client = VurioGrpcClient.shared.duplicates {
                let res = try await client.undoEjectDuplicates(req)
                if res.success {
                    group.files[selectedIndex].isEjected = false
                    onEjectStatusChanged?(file.filePath, false)
                }
            }
        } catch {
            print("Restore error: \(error)")
        }
    }
    
    private func resetTransform() {
        scale = 1.0
        rotation = 0
    }
}
