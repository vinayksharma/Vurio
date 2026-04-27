import SwiftUI

struct ImagePopupView: View {
    let group: Vurio_Dedup_DuplicateGroup
    @Binding var isPresented: Bool
    
    @State private var selectedIndex: Int = 0
    @State private var scale: CGFloat = 1.0
    @State private var rotation: Double = 0
    
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
                        let file = group.files[selectedIndex]
                        
                        if let image = NSImage(contentsOfFile: file.filePath) {
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
            }
            .padding()
            .background(Color(NSColor.windowBackgroundColor))
        }
        .frame(minWidth: 800, minHeight: 600)
    }
    
    private func resetTransform() {
        scale = 1.0
        rotation = 0
    }
}
